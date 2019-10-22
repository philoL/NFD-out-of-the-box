/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2019,  Regents of the University of California,
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
#include "rib/service.hpp"

#include <ndn-cxx/lp/empty-value.hpp>
#include <ndn-cxx/lp/prefix-announcement-header.hpp>
#include <ndn-cxx/lp/tags.hpp>

#include <boost/range/adaptor/reversed.hpp>

namespace nfd {
namespace fw {

NFD_LOG_INIT(SelfLearningStrategy);
NFD_REGISTER_STRATEGY(SelfLearningStrategy);

const time::milliseconds SelfLearningStrategy::RETX_SUPPRESSION_INITIAL(10);
const time::milliseconds SelfLearningStrategy::RETX_SUPPRESSION_MAX(250);
const int SelfLearningStrategy::RETX_TRIGGER_BROADCAST_COUNT(7);
const time::milliseconds SelfLearningStrategy::ROUTE_RENEW_LIFETIME(5_min);

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
    // initialize rtxCount in pitEntryInfo
    pitEntry->insertStrategyInfo<PitEntryInfo>();
    // find eligible nexthop with the lowest cost
    it = std::find_if(nexthops.begin(), nexthops.end(), [&] (const auto& nexthop) {
      return isNextHopEligible(ingress.face, interest, nexthop, pitEntry);
    });

    if (it == nexthops.end()) { // no next hop, do self-learning
      noNexthopHandler(ingress, interest, pitEntry);
    }
    else { // forward to nexthop with the lowest cost
      withNexthopHandler(ingress.face, it->getFace(), interest, pitEntry);
    }
  }
  else { // retransmitted Interest to be forwarded
    // find an unused upstream with lowest cost except downstream
    it = std::find_if(nexthops.begin(), nexthops.end(), [&] (const auto& nexthop) {
      return isNextHopEligible(ingress.face, interest, nexthop, pitEntry, true, time::steady_clock::now());
    });

    if (it == nexthops.end()) { // all next hops have been tried
      allNexthopTriedHandler(ingress, interest, pitEntry, nexthops);
    }
    else{
      withNexthopHandler(ingress.face, it->getFace(), interest, pitEntry);
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
      asyncProcessData(pitEntry, ingress, data);
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
    //NFD_LOG_DEBUG("Receive Data " << data << " without send the Interest out");
    //Strategy::afterReceiveData(pitEntry, ingress, data);
    auto paTag = data.getTag<lp::PrefixAnnouncementTag>();
    if (paTag != nullptr) { // Data contains PrefixAnnouncement
      if (ingress.face.getLinkType() == ndn::nfd::LINK_TYPE_MULTI_ACCESS) { // create unicast face
        NFD_LOG_DEBUG("Incoming face= " << ingress.face.getId() << " is multi-access, prelong pit entry expiry timer,"
                       << " and create unicast face");
        this->setExpiryTimer(pitEntry, 1_s);
        this->createUnicastFaceOnMulticast(pitEntry, ingress, data);
      }
      else {
        NFD_LOG_DEBUG("Incoming face= " << ingress.face.getId() << " is not multi-access, announce route to it");
        addRoute(pitEntry, ingress.face, data, *paTag->get().getPrefixAnn());
      }
    }
    else { // Data contains no PrefixAnnouncement, upstreams do not support self-learning
      NFD_LOG_DEBUG("Receive Data with out PA, upstreams do not support self-learning");
    }
    NFD_LOG_DEBUG("Send Data to all downstreams");
    sendDataToAll(pitEntry, ingress, data);
  }
  else {
    OutRecordInfo* outRecordInfo = outRecord->getStrategyInfo<OutRecordInfo>();
    if (outRecordInfo && outRecordInfo->isNonDiscoveryInterest) { // outgoing Interest was non-discovery
      if (!needPrefixAnn(pitEntry)) { // no need to attach a PA (common cases)
        NFD_LOG_DEBUG("Receive Data " << data << " to satisfy non-discovery Interest, no need to attach a PA");
        sendDataToAll(pitEntry, ingress, data);
      }
      else { // needs a PA (to respond discovery Interest)
        NFD_LOG_DEBUG("Receive Data " << data << " to satisfy discovery Interest, async processing");
        asyncProcessData(pitEntry, ingress, data);
      }
    }
    else { // outgoing Interest was discovery
      auto paTag = data.getTag<lp::PrefixAnnouncementTag>();
      if (paTag != nullptr) { // Data contains PrefixAnnouncement
        if (ingress.face.getLinkType() == ndn::nfd::LINK_TYPE_MULTI_ACCESS) { // create unicast face
          NFD_LOG_DEBUG("Incoming face= " << ingress.face.getId() << " is multi-access, prelong pit entry expiry timer,"
                         << " and create unicast face");
          this->setExpiryTimer(pitEntry, 1_s);
          this->createUnicastFaceOnMulticast(pitEntry, ingress, data);
        }
        else {
          NFD_LOG_DEBUG("Incoming face= " << ingress.face.getId() << " is not multi-access, announce route to it");
          addRoute(pitEntry, ingress.face, data, *paTag->get().getPrefixAnn());
        }
      }
      else { // Data contains no PrefixAnnouncement, upstreams do not support self-learning
        NFD_LOG_DEBUG("Receive Data with out PA, upstreams do not support self-learning");
      }
      NFD_LOG_DEBUG("Send Data to all downstreams");
      sendDataToAll(pitEntry, ingress, data);
    }
  }
}

void
SelfLearningStrategy::afterReceiveNack(const FaceEndpoint& ingress, const lp::Nack& nack,
                                       const shared_ptr<pit::Entry>& pitEntry)
{
  if (nack.getReason() == lp::NackReason::NO_ROUTE) { // remove FIB entries
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
           // no alternative path, send NACK to downstreams or braodcast discovery Interest at consumer
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
           withNexthopHandler(pitEntry->in_begin()->getFace(), it->getFace(), pitEntry->getInterest(), pitEntry);
         }
       }
       else { // outgoing Interest was discovery
         // should not happen
       }
     }
   }
}

void
SelfLearningStrategy::noNexthopHandler(const FaceEndpoint& ingress, const Interest& interest,
                                     const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_DEBUG("No next hop handler: Interest=" << interest);
  bool isNonDiscovery = interest.getTag<lp::NonDiscoveryTag>() != nullptr;
  auto inRecordInfo = pitEntry->getInRecord(ingress.face)->insertStrategyInfo<InRecordInfo>().first;
  inRecordInfo->isNonDiscoveryInterest = isNonDiscovery;

  if (isNonDiscovery) { // receive "non-discovery" Interest, send no-route NACK back
    NFD_LOG_DEBUG("NACK non-discovery Interest=" << interest << " from=" << ingress << " noNextHop");
    lp::NackHeader nackHeader;
    nackHeader.setReason(lp::NackReason::NO_ROUTE);
    this->sendNack(pitEntry, ingress, nackHeader);
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
    auto egress = FaceEndpoint(it->getFace(), 0);
    this->sendInterest(pitEntry, egress, interest);
    NFD_LOG_DEBUG(interest << " from=" << ingress << " retransmit-retry-to=" << egress);
  }

  // auto inRecordInfo = pitEntry->getInRecord(ingress.face)->insertStrategyInfo<InRecordInfo>().first;
  // inRecordInfo->isNonDiscoveryInterest = interest.getTag<lp::NonDiscoveryTag>() != nullptr;

  // auto pitEntryInfo = pitEntry->getStrategyInfo<PitEntryInfo>();
  // ++pitEntryInfo->rtxCount;
  // NFD_LOG_DEBUG("Retransmission Interest= " << interest  << "---> "<< pitEntryInfo->rtxCount <<" times");

  // if (pitEntryInfo->rtxCount == 1) {
  //   NFD_LOG_DEBUG("find an eligible upstream that is used earliest Interest= " << interest);
  //   auto it = findEligibleNextHopWithEarliestOutRecord(ingress.face, interest, nexthops, pitEntry);
  //   if (it == nexthops.end()) {
  //     NFD_LOG_DEBUG(interest << " from=" << ingress << " retransmitNoNextHop");
  //   }
  //   else {
  //     auto egress = FaceEndpoint(it->getFace(), 0);
  //     this->sendInterest(pitEntry, egress, interest);
  //     NFD_LOG_DEBUG(interest << " from=" << ingress << " retransmit-retry-to=" << egress);
  //   }
  // }
  // else if (pitEntryInfo->rtxCount == RETX_TRIGGER_BROADCAST_COUNT) {
  //   NFD_LOG_DEBUG("Interest retransmissions reach " << RETX_TRIGGER_BROADCAST_COUNT << " times, clear route and reinitiate flooding");
  //   // retransmission triggers broadcasting discovery Interest and clearing FIB entries
  //   pitEntryInfo->rtxCount = 0;
  //   inRecordInfo->isNonDiscoveryInterest = false;
  //   interest.removeTag<lp::NonDiscoveryTag>();

  //   const fib::Entry& fibEntry = this->lookupFib(*pitEntry);
  //   const fib::NextHopList& nexthops = fibEntry.getNextHops();
  //   for (const auto& nexthop : nexthops) {
  //     renewRoute(interest.getName(), nexthop.getFace().getId(), 0_ms);
  //   }

  //   broadcastInterest(interest, ingress.face, pitEntry);
  // }
}

void
SelfLearningStrategy::withNexthopHandler(const Face& inFace, const Face& outFace, const Interest& interest,
                                       const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_DEBUG("with next hop hander Interest=" << interest);
  bool isNonDiscovery = interest.getTag<lp::NonDiscoveryTag>() != nullptr;
  auto inRecordInfo = pitEntry->getInRecord(inFace)->insertStrategyInfo<InRecordInfo>().first;
  inRecordInfo->isNonDiscoveryInterest = isNonDiscovery;
  if (!isNonDiscovery) {
    interest.setTag(make_shared<lp::NonDiscoveryTag>(lp::EmptyValue{}));
  }
  this->sendInterest(pitEntry, FaceEndpoint(outFace, 0), interest);
  pitEntry->getOutRecord(outFace)->insertStrategyInfo<OutRecordInfo>().first->isNonDiscoveryInterest = true;
  NFD_LOG_DEBUG("Send Interest " << interest << " to=" << outFace.getId());
}

void
SelfLearningStrategy::broadcastInterest(const Interest& interest, const Face& inFace,
                                        const shared_ptr<pit::Entry>& pitEntry)
{
  NFD_LOG_DEBUG("broadcast Interest");
  for (auto& outFace : this->getFaceTable()) {
    if ((outFace.getId() == inFace.getId() && outFace.getLinkType() != ndn::nfd::LINK_TYPE_AD_HOC) ||
        wouldViolateScope(inFace, interest, outFace) || outFace.getScope() == ndn::nfd::FACE_SCOPE_LOCAL) {
      continue;
    }
    this->sendInterest(pitEntry, FaceEndpoint(outFace, 0), interest);
    pitEntry->getOutRecord(outFace)->insertStrategyInfo<OutRecordInfo>().first->isNonDiscoveryInterest = false;
    NFD_LOG_DEBUG("send discovery Interest=" << interest << " from="
                  << inFace.getId() << " to=" << outFace.getId());
  }
}


void
SelfLearningStrategy::asyncProcessData(const shared_ptr<pit::Entry>& pitEntry, const FaceEndpoint& ingress, const Data& data)
{
  // Given that this processing is asynchronous, the PIT entry's expiry timer is extended first
  // to ensure that the entry will not be removed before the whole processing is finished
  // (the PIT entry's expiry timer was set to 0 before dispatching)
  this->setExpiryTimer(pitEntry, 1_s);

  runOnRibIoService([pitEntryWeak = weak_ptr<pit::Entry>{pitEntry}, inFaceId = ingress.face.getId(), data, this] {
    rib::Service::get().getRibManager().slFindAnn(data.getName(),
      [pitEntryWeak, inFaceId, data, this] (optional<ndn::PrefixAnnouncement> paOpt) {
        if (paOpt) {
          runOnMainIoService([pitEntryWeak, inFaceId, data, pa = std::move(*paOpt), this] {
            auto pitEntry = pitEntryWeak.lock();
            auto inFace = this->getFace(inFaceId);
            if (pitEntry && inFace) {
              NFD_LOG_DEBUG("found PrefixAnnouncement=" << pa.getAnnouncedName());
              data.setTag(make_shared<lp::PrefixAnnouncementTag>(lp::PrefixAnnouncementHeader(pa)));
              this->sendDataToAll(pitEntry, FaceEndpoint(*inFace, 0), data);
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
SelfLearningStrategy::afterUnicastFaceCreationSuccess(const shared_ptr<pit::Entry>& pitEntry, const FaceEndpoint& ingress,
                                                      const Face& face, const Data& data)
{
  NFD_LOG_DEBUG("Unicast face created=" << face.getId() << ", announce route towards it");
  auto paTag = data.getTag<lp::PrefixAnnouncementTag>();
  addRoute(pitEntry, face, data, *paTag->get().getPrefixAnn());
  sendDataToAll(pitEntry, ingress, data);
}

void
SelfLearningStrategy::afterUnicastFaceCreationFailure(const shared_ptr<pit::Entry>& pitEntry,
                                                      const FaceEndpoint& ingress, const Data& data)
{
  NFD_LOG_DEBUG("Unicast face creation failed, no route created");
  //auto paTag = data.getTag<lp::PrefixAnnouncementTag>();
  //addRoute(pitEntry, ingress.face, data, *paTag->get().getPrefixAnn());
  sendDataToAll(pitEntry, ingress, data);
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
