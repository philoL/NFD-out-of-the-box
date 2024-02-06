/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2022,  Regents of the University of California,
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

#ifndef NFD_DAEMON_FW_SELF_LEARNING_STRATEGY_HPP
#define NFD_DAEMON_FW_SELF_LEARNING_STRATEGY_HPP

#include "fw/strategy.hpp"
#include "process-nack-traits.hpp"
#include "retx-suppression-exponential.hpp"

#include <ndn-cxx/prefix-announcement.hpp>

namespace nfd::fw {

/**
 * \brief Self-learning forwarding strategy.
 *
 *  This strategy forwards Interests in round-robin manner according the ranking of next hops,
 *  with Interest suppression and retransmission mechanisms added.
 *  In addition, when no next hop is found in FIB, the Interest will be broadcast to non-local faces.
 *
 *  On receiving Data for broadcast Interest, a route will be added to FIB according to the Prefix Announcement
 *  attached to Data. In addition, unicast face will be created when receiving data from a multicast face.
 *
 *  \see https://github.com/philoL/NDN-Self-Learning/blob/master/self-learning-v2.pdf
 */
class SelfLearningStrategy : public Strategy
                           , public ProcessNackTraits<SelfLearningStrategy>
{
public:
  explicit
  SelfLearningStrategy(Forwarder& forwarder, const Name& name = getStrategyName());

  static const Name&
  getStrategyName();

  /// StrategyInfo on pit::InRecord
  class InRecordInfo final : public StrategyInfo
  {
  public:
    static constexpr int
    getTypeId()
    {
      return 1040;
    }

  public:
    bool isNonDiscoveryInterest = false;
  };

  /// StrategyInfo on pit::OutRecord
  class OutRecordInfo final : public StrategyInfo
  {
  public:
    static constexpr int
    getTypeId()
    {
      return 1041;
    }

  public:
    bool isNonDiscoveryInterest = false;
  };

public: // triggers
  void
  afterReceiveInterest(const Interest& interest, const FaceEndpoint& ingress,
                       const shared_ptr<pit::Entry>& pitEntry) override;

  void
  afterContentStoreHit(const Data& data, const FaceEndpoint& ingress,
                       const shared_ptr<pit::Entry>& pitEntry) override;

  void
  afterReceiveData(const Data& data, const FaceEndpoint& ingress,
                   const shared_ptr<pit::Entry>& pitEntry) override;

  void
  afterReceiveNack(const lp::Nack& nack, const FaceEndpoint& ingress,
                   const shared_ptr<pit::Entry>& pitEntry) override;

private: // operations
  /** \brief Send an Interest to all possible faces.
   *
   *  This function is invoked when the forwarder has no matching FIB entries for
   *  an incoming discovery Interest, which will be forwarded to faces that
   *    - do not violate the Interest scope
   *    - are non-local
   *    - are not the face from which the Interest arrived, unless the face is ad-hoc
   */
  void
  broadcastInterest(const Interest& interest, const Face& inFace,
                    const shared_ptr<pit::Entry>& pitEntry);

  void
  noNexthopHandler(const FaceEndpoint& ingress, const Interest& interest,
                   const shared_ptr<pit::Entry>& pitEntry);

  void
  allNexthopTriedHandler(const FaceEndpoint& ingress, const Interest& interest,
                         const shared_ptr<pit::Entry>& pitEntry, const fib::NextHopList& nexthops);

  void
  hasUntriedNexthopHandler(const FaceEndpoint& ingress, Face& outFace, const Interest& interest,
                           const shared_ptr<pit::Entry>& pitEntry);

  /** \brief Find a Prefix Announcement for the Data on the RIB thread, and forward
   *         the Data with the Prefix Announcement on the main thread.
   */
  void
  asyncProcessData(const shared_ptr<pit::Entry>& pitEntry, const Face& inFace, const Data& data);

  /** \brief Check whether a PrefixAnnouncement needs to be attached to an incoming Data.
   *
   *  The conditions that a Data packet requires a PrefixAnnouncement are
   *    - the incoming Interest was discovery and
   *    - the outgoing Interest was non-discovery and
   *    - this forwarder does not directly connect to the consumer
   */
  static bool
  needPrefixAnn(const shared_ptr<pit::Entry>& pitEntry);

  /** \brief Add a route using RibManager::slAnnounce on the RIB thread.
   */
  void
  addRoute(const shared_ptr<pit::Entry>& pitEntry, const Face& inFace,
           const Data& data, const ndn::PrefixAnnouncement& pa);

  /** \brief Renew a route using RibManager::slRenew on the RIB thread.
   */
  void
  renewRoute(const Name& name, FaceId inFaceId, time::milliseconds maxLifetime);

private:
  bool
  isThisConsumer(const shared_ptr<pit::Entry>& pitEntry);

NFD_PUBLIC_WITH_TESTS_ELSE_PRIVATE:
  static const time::milliseconds ROUTE_RENEW_LIFETIME;
  static const time::milliseconds RETX_SUPPRESSION_INITIAL;
  static const time::milliseconds RETX_SUPPRESSION_MAX;
  static const int RETX_TRIGGER_BROADCAST_COUNT;
  RetxSuppressionExponential m_retxSuppression;

  friend ProcessNackTraits<SelfLearningStrategy>;
};

} // namespace nfd::fw

#endif // NFD_DAEMON_FW_SELF_LEARNING_STRATEGY_HPP
