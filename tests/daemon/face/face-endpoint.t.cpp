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

#include "face/face-endpoint.hpp"

#include "face/ethernet-protocol.hpp"
#include "face/face-common.hpp"
#include "face/udp-protocol.hpp"
#include "tests/test-common.hpp"
#include "dummy-face.hpp"
#include <boost/lexical_cast.hpp>

namespace nfd {
namespace face {
namespace tests {

BOOST_AUTO_TEST_SUITE(Face)
BOOST_AUTO_TEST_SUITE(TestFaceEndpoint)

BOOST_AUTO_TEST_CASE(OutputStream)
{
  auto face1 = make_shared<DummyFace>();
  FaceEndpoint faceEndpoint1(*face1);
  BOOST_CHECK_EQUAL(boost::lexical_cast<std::string>(faceEndpoint1), "0");

  auto face2 = make_shared<DummyFace>();
  udp::Endpoint udpEp({boost::asio::ip::address_v4(0xe00017aa), 56363});
  EndpointId ep1 = udpEp;
  FaceEndpoint faceEndpoint2(*face2, ep1);
  BOOST_CHECK_EQUAL(boost::lexical_cast<std::string>(faceEndpoint2), "(0, 224.0.23.170:56363)");

  auto face3 = make_shared<DummyFace>();
  ethernet::Address ethEp({0x01, 0x00, 0x5e, 0x90, 0x10, 0x01});
  EndpointId ep2 = ethEp;
  FaceEndpoint faceEndpoint3(*face3, ep2);
  BOOST_CHECK_EQUAL(boost::lexical_cast<std::string>(faceEndpoint3), "(0, 01:00:5e:90:10:01)");
}

BOOST_AUTO_TEST_SUITE_END() // TestFaceEndpoint
BOOST_AUTO_TEST_SUITE_END() // Face

} // namespace tests
} // namespace face
} // namespace nfd
