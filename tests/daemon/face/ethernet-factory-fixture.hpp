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

#ifndef NFD_TESTS_DAEMON_FACE_ETHERNET_FACTORY_FIXTURE_HPP
#define NFD_TESTS_DAEMON_FACE_ETHERNET_FACTORY_FIXTURE_HPP

#include "face/ethernet-factory.hpp"

#include "ethernet-fixture.hpp"
#include "face-system-fixture.hpp"
#include "factory-test-common.hpp"

#include <boost/algorithm/string/replace.hpp>

namespace nfd {
namespace face {
namespace tests {

using namespace nfd::tests;

class EthernetFactoryFixture : public EthernetFixture
                             , public FaceSystemFactoryFixture<EthernetFactory>
{
protected:
  EthernetFactoryFixture()
  {
    this->copyRealNetifsToNetmon();
  }

  std::set<std::string>
  listUrisOfAvailableNetifs() const
  {
    std::set<std::string> uris;
    std::transform(netifs.begin(), netifs.end(), std::inserter(uris, uris.end()),
                   [] (const auto& netif) {
                     return FaceUri::fromDev(netif->getName()).toString();
                   });
    return uris;
  }

  std::vector<const Face*>
  listEtherMcastFaces(ndn::nfd::LinkType linkType = ndn::nfd::LINK_TYPE_MULTI_ACCESS) const
  {
    return this->listFacesByScheme("ether", linkType);
  }

  size_t
  countEtherMcastFaces(ndn::nfd::LinkType linkType = ndn::nfd::LINK_TYPE_MULTI_ACCESS) const
  {
    return this->listEtherMcastFaces(linkType).size();
  }
};

} // namespace tests
} // namespace face
} // namespace nfd

#endif // NFD_TESTS_DAEMON_FACE_ETHERNET_FACTORY_FIXTURE_HPP
