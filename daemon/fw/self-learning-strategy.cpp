/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2020,  Regents of the University of California,
 *                           Arizona Board of Regents,
 *                           Colorado State University,
 *                           University Pierre & Marie Curie, Sorbonne University,
 *                           Washington University in St. Louis,
 *                           Beijing Institute of Technology,
 *                           The University of Memphis.
 *
 * This file is part of NFD (Named Data Networking Forwarding Daemon).
 * See AUTHORS.md for complete list of NFD authors and contributors.
 *
 * NFD is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * NFD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * NFD, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "self-learning-strategy.hpp"
#include "algorithm.hpp"

#include "common/global.hpp"
#include "common/logger.hpp"
#include "face/channel.hpp"
#include "face/face-common.hpp"
#include "rib/service.hpp"

#include <ndn-cxx/lp/empty-value.hpp>
#include <ndn-cxx/lp/prefix-announcement-header.hpp>
#include <ndn-cxx/lp/tags.hpp>

#include <boost/range/adaptor/reversed.hpp>

namespace nfd {
namespace fw {

NFD_LOG_INIT(SelfLearningStrategy);
NFD_REGISTER_STRATEGY(SelfLearningStrategy);

const time::milliseconds SelfLearningStrategy::ROUTE_RENEW_LIFETIME(10_min);
const time::milliseconds SelfLearningStrategy::RETX_SUPPRESSION_INITIAL(10);
const time::milliseconds SelfLearningStrategy::RETX_SUPPRESSION_MAX(250);
const int SelfLearningStrategy::RETX_TRIGGER_BROADCAST_COUNT(7);

SelfLearningStrategy::SelfLearningStrategy(Forwarder& forwarder, const Name& name)
  : Strategy(forwarder)
  , ProcessNackTraits(this)
  , m_retxSuppression(RETX_SUPPRESSION_INITIAL,
                      RetxSuppressionExponential::DEFAULT_MULTIPLIER,
                      RETX_SUPPRESSION_MAX)
{
  ParsedInstanceName parsed = parseInstanceName(name);
  if (!parsed.parameters.empty()) {
    NDN_THROW(std::invalid_argument("SelfLearningStrategy does not accept parameters"));
  }
  if (parsed.version && *parsed.version != getStrategyName()[-1].toVersion()) {
    NDN_THROW(std::invalid_argument(
      "SelfLearningStrategy does not support version " + to_string(*parsed.version)));
  }
  this->setInstanceName(makeInstanceName(name, getStrategyName()));
}

const Name&
SelfLearningStrategy::getStrategyName()
{
  static Name strategyName("/localhost/nfd/strategy/self-learning/%FD%02");
  return strategyName;
}

void
SelfLearningStrategy::afterReceiveInterest(const FaceEndpoint& ingress, const Interest& interest,
                                           const shared_ptr<pit::Entry>& pitEntry)
{
  RetxSuppressionResult suppression = m_retxSuppression.decidePerPitEntry(*pitEntry);
  if (suppression == RetxSuppressionResult::SUPPRESS) {
    NFD_LOG_DEBUG(interest << " from=" << ingress << " suppressed");
    return;
  }

  const fib::Entry& fibEntry = this->lookupFib(*pitEntry);
  const fib::NextHopList& nexthops = fibEntry.getNextHops();

  auto it = nexthops.end();
  if (suppression == RetxSuppressionResult::NEW) { // new Interest
    // find eligible nexthop with the lowest cost
    it = std::find_if(nexthops.begin(), nexthops.end(), [&] (const auto& nexthop) {
      return isNextHopEligible(ingress.face, interest, nexthop, pitEntry);
    });

    if (it == nexthops.end()) { // no next hop, do self-learning
      noNexthopHandler(ingress, interest, pitEntry);
    }
    else { // forward to nexthop with the lowest cost
      hasUntriedNexthopHandler(ingress, it->getFace(), interest, pitEntry);
    }
  }
  else { // retransmitted Interest to be forwarded
    // find an unused upstream with the lowest cost except the downstream
    it = std::find_if(nexthops.begin(), nexthops.end(), [&] (const auto& nexthop) {
      return isNextHopEligible(ingress.face, interest, nexthop, pitEntry, true, time::steady_clock::now());
    });

    if (it == nexthops.end()) { // all next hops have been tried
      allNexthopTriedHandler(ingress, interest, pitEntry, nexthops);
    }
    else{
      hasUntriedNexthopHandler(ingress, it->getFace(), interest, pitEntry);
    }
  }
}

void
SelfLearningStrategy::afterContentStoreHit(const shared_ptr<pit::Entry>& pitEntry,
                                           const FaceEndpoint& ingress, const Data& data)
{
  NFD_LOG_DEBUG("after cs hit");
  if (ingress.face.getScope() == ndn::nfd::FACE_SCOPE_LOCAL) {
    NFD_LOG_DEBUG("this is consumer");
    Strategy::afterContentStoreHit(pitEntry, ingress, data);
  }
  else {
    // if interest is discovery Interest, and data does not contain a PA, attach a PA to it
    bool isNonDiscovery = pitEntry->getInterest().getTag<lp::NonDiscoveryTag>() != nullptr;
    auto paTag = data.getTag<lp::PrefixAnnouncementTag>();
    if (not isNonDiscovery && not paTag) {
      NFD_LOG_DEBUG("find pa");
      asyncProcessData(pitEntry, ingress.face, data);
    }
    else {
      NFD_LOG_DEBUG("no need to find pa");
      Strategy::afterContentStoreHit(pitEntry, ingress, data);
    }
  }
}

void
SelfLearningStrategy::afterReceiveData(const shared_ptr<pit::Entry>& pitEntry,
                                       const FaceEndpoint& ingress, const Data& data)
{
  auto outRecord = pitEntry->getOutRecord(ingress.face);
  if (outRecord == pitEntry->out_end()) {
    NFD_LOG_DEBUG("Data " << data.getName() << " from=" << ingress << " no out-record");
    return;
  }

  OutRecordInfo* outRecordInfo = outRecord->getStrategyInfo<OutRecordInfo>();
  if (outRecordInfo && outRecordInfo->isNonDiscoveryInterest) { // outgoing Interest was non-discovery
    if (!needPrefixAnn(pitEntry)) { // no need to attach a PA (common cases)
      sendDataToAll(pitEntry, ingress.face, data);
    }
    else { // needs a PA (to respond discovery Interest)
      asyncProcessData(pitEntry, ingress.face, data);
    }
  }
  else { // outgoing Interest was discovery
    auto paTag = data.getTag<lp::PrefixAnnouncementTag>();
    if (paTag != nullptr) {
      if (ingress.face.getLinkType() == ndn::nfd::LINK_TYPE_MULTI_ACCESS) { // create unicast face
        NFD_LOG_DEBUG("Incoming face= " << ingress.face.getId() << " is multi-access, connect to the unicast face");
        shared_ptr<face::Channel> channel = ingress.face.getChannel().lock();
        face::FaceParams faceParams;
        faceParams.persistency = ndn::nfd::FACE_PERSISTENCY_ON_DEMAND;
        channel->connect(ingress.endpoint, faceParams,
          [&] (const shared_ptr<nfd::Face>& face) {
            NFD_LOG_DEBUG("unicast face created, add route");
            this->addFace(face);
            addRoute(pitEntry, *face, data, *paTag->get().getPrefixAnn());
          },
          [] (uint32_t, const std::string& reason) {
            NFD_LOG_DEBUG("unicast face creation failied, reason= " << reason);
          });
      }
      else {
        NFD_LOG_DEBUG("Incoming face= " << ingress.face.getId() << " is not multi-access, announce route to it");
        addRoute(pitEntry, ingress.face, data, *paTag->get().getPrefixAnn());
      }
    }
    else { // Data contains no PrefixAnnouncement, upstreams do not support self-learning
    }
    sendDataToAll(pitEntry, ingress.face, data);
  }
}

void
SelfLearningStrategy::afterReceiveNack(const FaceEndpoint& ingress, const lp::Nack& nack,
                                       const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_DEBUG("Nack for " << nack.getInterest() << " from=" << ingress << " reason=" << nack.getReason());
  if (nack.getReason() == lp::NackReason::NO_ROUTE) { // remove the FIB entry
    renewRoute(nack.getInterest().getName(), ingress.face.getId(), 0_ms);
    auto outRecord = pitEntry->getOutRecord(ingress.face);
    if (outRecord == pitEntry->out_end()) { // should not happen with correct behaviours
      NFD_LOG_DEBUG("Receive no-route NACK for an unsent Interest");
      return;
    }
    else {
      NFD_LOG_DEBUG("Receive no-route NACK from=" << ingress.face.getId());
      OutRecordInfo* outRecordInfo = outRecord->getStrategyInfo<OutRecordInfo>();
      if (outRecordInfo && outRecordInfo->isNonDiscoveryInterest) {
        // outgoing Interest was nondiscovery, try an unused next hops
        const fib::Entry& fibEntry = this->lookupFib(*pitEntry);
        const fib::NextHopList& nexthops = fibEntry.getNextHops();
        auto it = std::find_if(nexthops.begin(), nexthops.end(), [&] (const auto& nexthop) {
          return isNextHopEligible(ingress.face, pitEntry->getInterest(), nexthop, pitEntry, true, time::steady_clock::now());
        });
        if (it == nexthops.end()) {
          // no untried path, send NACK to downstreams or braodcast discovery Interest at consumer
          if (isThisConsumer(pitEntry)) {
            auto inRecordInfo = pitEntry->in_begin()->insertStrategyInfo<InRecordInfo>().first;
            inRecordInfo->isNonDiscoveryInterest = false;
            auto interest = pitEntry->getInterest();
            interest.removeTag<lp::NonDiscoveryTag>();
            broadcastInterest(interest, pitEntry->in_begin()->getFace(), pitEntry);
          }
          else {
            this->processNack(ingress.face, nack, pitEntry);
          }
        }
        else {
          hasUntriedNexthopHandler(ingress, it->getFace(), pitEntry->getInterest(), pitEntry);
        }
      }
      else { // outgoing Interest was discovery
        // should not happen
      }
    }
  }
  this->processNack(ingress.face, nack, pitEntry);
}

void
SelfLearningStrategy::broadcastInterest(const Interest& interest, const Face& inFace,
                                        const shared_ptr<pit::Entry>& pitEntry)
{
  for (auto& outFace : this->getFaceTable() | boost::adaptors::reversed) {
    if ((outFace.getId() == inFace.getId() && outFace.getLinkType() != ndn::nfd::LINK_TYPE_AD_HOC) ||
        wouldViolateScope(inFace, interest, outFace) || outFace.getScope() == ndn::nfd::FACE_SCOPE_LOCAL) {
      continue;
    }
    this->sendInterest(pitEntry, outFace, interest);
    pitEntry->getOutRecord(outFace)->insertStrategyInfo<OutRecordInfo>().first->isNonDiscoveryInterest = false;
    NFD_LOG_DEBUG("send discovery Interest=" << interest << " from="
                  << inFace.getId() << " to=" << outFace.getId());
  }
}

void
SelfLearningStrategy::noNexthopHandler(const FaceEndpoint& ingress, const Interest& interest,
                                       const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_DEBUG("No next hop found, broadcast Interest=" << interest);
  bool isNonDiscovery = interest.getTag<lp::NonDiscoveryTag>() != nullptr;
  auto inRecordInfo = pitEntry->getInRecord(ingress.face)->insertStrategyInfo<InRecordInfo>().first;
  inRecordInfo->isNonDiscoveryInterest = isNonDiscovery;

  if (isNonDiscovery) { // receive "non-discovery" Interest, send no-route NACK back
    NFD_LOG_DEBUG("NACK non-discovery Interest=" << interest << " from=" << ingress << " noNextHop");
    lp::NackHeader nackHeader;
    nackHeader.setReason(lp::NackReason::NO_ROUTE);
    this->sendNack(pitEntry, ingress.face, nackHeader);
    this->rejectPendingInterest(pitEntry);
    return;
  }
  else { // receive "discovery" Interest, broadcast it
    broadcastInterest(interest, ingress.face, pitEntry);
    return;
  }
}

void
SelfLearningStrategy::allNexthopTriedHandler(const FaceEndpoint& ingress, const Interest& interest,
                                             const shared_ptr<pit::Entry>& pitEntry, const fib::NextHopList& nexthops)
{
  NFD_LOG_DEBUG("all nexthops have been tried, forward in round-robin manner");
  auto it = findEligibleNextHopWithEarliestOutRecord(ingress.face, interest, nexthops, pitEntry);
  if (it == nexthops.end()) {
    NFD_LOG_DEBUG(interest << " from=" << ingress << " retransmitNoNextHop");
  }
  else {
    this->sendInterest(pitEntry, it->getFace(), interest);
    NFD_LOG_DEBUG(interest << " from=" << ingress << " retransmit-retry-to Face=" << it->getFace().getId());
  }
}

void
SelfLearningStrategy::hasUntriedNexthopHandler(const FaceEndpoint& ingress, Face& outFace, const Interest& interest,
                                               const shared_ptr<pit::Entry>& pitEntry)
{
  bool isNonDiscovery = interest.getTag<lp::NonDiscoveryTag>() != nullptr;
  auto inRecordInfo = pitEntry->getInRecord(ingress.face)->insertStrategyInfo<InRecordInfo>().first;
  inRecordInfo->isNonDiscoveryInterest = isNonDiscovery;
  if (!isNonDiscovery) {
    interest.setTag(make_shared<lp::NonDiscoveryTag>(lp::EmptyValue{}));
  }
  this->sendInterest(pitEntry, outFace, interest);
  pitEntry->getOutRecord(outFace)->insertStrategyInfo<OutRecordInfo>().first->isNonDiscoveryInterest = true;
  NFD_LOG_DEBUG("Send Interest " << interest << " to the untried Face=" << outFace.getId());
}

void
SelfLearningStrategy::asyncProcessData(const shared_ptr<pit::Entry>& pitEntry, const Face& inFace, const Data& data)
{
  // Given that this processing is asynchronous, the PIT entry's expiry timer is extended first
  // to ensure that the entry will not be removed before the whole processing is finished
  // (the PIT entry's expiry timer was set to 0 before dispatching)
  this->setExpiryTimer(pitEntry, 1_s);

  runOnRibIoService([pitEntryWeak = weak_ptr<pit::Entry>{pitEntry}, inFaceId = inFace.getId(), data, this] {
    rib::Service::get().getRibManager().slFindAnn(data.getName(),
      [pitEntryWeak, inFaceId, data, this] (optional<ndn::PrefixAnnouncement> paOpt) {
        if (paOpt) {
          runOnMainIoService([pitEntryWeak, inFaceId, data, pa = std::move(*paOpt), this] {
            auto pitEntry = pitEntryWeak.lock();
            auto inFace = this->getFace(inFaceId);
            if (pitEntry && inFace) {
              NFD_LOG_DEBUG("found PrefixAnnouncement=" << pa.getAnnouncedName());
              data.setTag(make_shared<lp::PrefixAnnouncementTag>(lp::PrefixAnnouncementHeader(pa)));
              this->sendDataToAll(pitEntry, *inFace, data);
              this->setExpiryTimer(pitEntry, 0_ms);
            }
            else {
              NFD_LOG_DEBUG("PIT entry or Face no longer exists");
            }
          });
        }
    });
  });
}

bool
SelfLearningStrategy::needPrefixAnn(const shared_ptr<pit::Entry>& pitEntry)
{
  bool hasDiscoveryInterest = false;
  bool directToConsumer = true;

  auto now = time::steady_clock::now();
  for (const auto& inRecord : pitEntry->getInRecords()) {
    if (inRecord.getExpiry() > now) {
      InRecordInfo* inRecordInfo = inRecord.getStrategyInfo<InRecordInfo>();
      if (inRecordInfo && !inRecordInfo->isNonDiscoveryInterest) {
        hasDiscoveryInterest = true;
      }
      if (inRecord.getFace().getScope() != ndn::nfd::FACE_SCOPE_LOCAL) {
        directToConsumer = false;
      }
    }
  }
  return hasDiscoveryInterest && !directToConsumer;
}

void
SelfLearningStrategy::addRoute(const shared_ptr<pit::Entry>& pitEntry, const Face& inFace,
                               const Data& data, const ndn::PrefixAnnouncement& pa)
{
  runOnRibIoService([pitEntryWeak = weak_ptr<pit::Entry>{pitEntry}, inFaceId = inFace.getId(), data, pa] {
    rib::Service::get().getRibManager().slAnnounce(pa, inFaceId, ROUTE_RENEW_LIFETIME,
      [] (RibManager::SlAnnounceResult res) {
        NFD_LOG_DEBUG("Add route via PrefixAnnouncement with result=" << res);
      });
  });
}

void
SelfLearningStrategy::renewRoute(const Name& name, FaceId inFaceId, time::milliseconds maxLifetime)
{
  // renew route with PA or ignore PA (if route has no PA)
  runOnRibIoService([name, inFaceId, maxLifetime] {
    rib::Service::get().getRibManager().slRenew(name, inFaceId, maxLifetime,
      [] (RibManager::SlAnnounceResult res) {
        NFD_LOG_DEBUG("Renew route with result=" << res);
      });
  });
}

bool
SelfLearningStrategy::isThisConsumer(const shared_ptr<pit::Entry>& pitEntry)
{
  const pit::InRecordCollection& inRecords = pitEntry->getInRecords();
  if (inRecords.size() == 1 && inRecords.front().getFace().getScope() == ndn::nfd::FACE_SCOPE_LOCAL) {
    return true;
  }
  return false;
}

} // namespace fw
} // namespace nfd
