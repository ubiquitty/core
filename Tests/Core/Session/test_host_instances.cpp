// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/HostInstanceInfo.h"
#include "Harness.h"

TEST(discoveredHostsRejectUnboundedMalformedAndDuplicateEndpoints)
{
    using namespace Ubiquitty;
    using Codec::JSON::Value;
    const auto endpoint = [](std::string socket, std::string executable, double time) {
        auto value = Value::object();
        value.set("socket", Value::string(std::move(socket)));
        value.set("executable", Value::string(std::move(executable)));
        value.set("startedUnixSeconds", Value::number(time));
        return value;
    };
    auto list = Value::array();
    list.push(endpoint("host-0123456789abcdef.sock", "/tmp/ubiquitty-host", 7));
    list.push(endpoint("host-0123456789abcdef.sock", "/tmp/another/ubiquitty-host", 8));
    list.push(endpoint("../host-0123456789abcdef.sock", "/tmp/ubiquitty-host", 7));
    list.push(endpoint("host-1111111111111111.sock", "relative/ubiquitty-host", 7));
    list.push(endpoint("host-2222222222222222.sock", "/tmp/ubiquitty-host", 7.5));
    const auto read = Session::readHostInstances(&list);
    CHECK_EQ(read.size(), 1u);
    CHECK(read.front().executable == "/tmp/ubiquitty-host");
    CHECK_EQ(read.front().startedUnixSeconds, 7u);
    for (std::size_t i = list.items().size(); i <= Session::maxDiscoveredHostInstances; ++i)
        list.push(Value::object());
    CHECK(Session::readHostInstances(&list).empty());
}

TEST(daemonVersionsBelongToTheAuthenticatedProcessAndAreBounded)
{
    using namespace Ubiquitty::Session;
    const std::string startup = "ubiquitty-host: daemon 0.2.0 (example) serving protocol 1, process 42, started 2026-10-27T02:37:32Z\n";
    CHECK_STR_EQ(daemonVersionFromLog(startup, 42), "0.2.0 (example)");
    CHECK(daemonVersionFromLog(startup, 4).empty());
    CHECK(daemonVersionFromLog(startup, 0).empty());
    CHECK(daemonVersionFromLog(std::string_view(startup).substr(0, startup.size() - 1), 42).empty());
    CHECK(daemonVersionFromLog("other: " + startup, 42).empty());
    CHECK(daemonVersionFromLog("ubiquitty-host: daemon " + std::string(129, 'x') + " serving protocol 1, process 42, started now\n", 42).empty());
    CHECK(!isValidDaemonVersion("0.2.0\033[2J"));
    CHECK(!isValidDaemonVersion("0.2.0\rspoof"));
}

TEST(discoveredDaemonVersionSurvivesTheDirectoryRecord)
{
    using namespace Ubiquitty;
    auto item = Codec::JSON::Value::object();
    item.set("socket", Codec::JSON::Value::string("host-0123456789abcdef.sock"));
    item.set("executable", Codec::JSON::Value::string("/tmp/ubiquitty-host"));
    item.set("appVersion", Codec::JSON::Value::string("0.2.0 (example)"));
    auto list = Codec::JSON::Value::array();
    list.push(item);
    const auto instances = Session::readHostInstances(&list);
    REQUIRE(instances.size() == 1);
    CHECK_STR_EQ(instances.front().appVersion, "0.2.0 (example)");
    auto oversized = Codec::JSON::Value::object();
    oversized.set("socket", Codec::JSON::Value::string("host-0123456789abcdef.sock"));
    oversized.set("executable", Codec::JSON::Value::string("/tmp/ubiquitty-host"));
    oversized.set("appVersion", Codec::JSON::Value::string(std::string(129, 'x')));
    auto invalid = Codec::JSON::Value::array();
    invalid.push(std::move(oversized));
    CHECK(Session::readHostInstances(&invalid).empty());
}

UBIQUITTY_TEST_MAIN()
