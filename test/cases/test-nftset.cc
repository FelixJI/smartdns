/*************************************************************************
 *
 * Copyright (C) 2018-2025 Ruilin Peng (Nick) <pymumu@gmail.com>.
 *
 * smartdns is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * smartdns is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "gtest/gtest.h"
#include "dns_server/cache.h"
#include "dns_server/context.h"
#include "smartdns/lib/nftset.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>
#include <endian.h>
#include <errno.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr int kPayloadMax = 2048;

struct NlmsgRequest {
	struct nlmsghdr header;
	struct nfgenmsg message;
};

struct rtattr *AddAttribute(struct nlmsghdr *header, int max_length, uint16_t type, const void *data, uint16_t length)
{
	uint16_t attribute_length = RTA_LENGTH(length);
	int new_length = NLMSG_ALIGN(header->nlmsg_len) + RTA_ALIGN(attribute_length);
	if (new_length > max_length) {
		return nullptr;
	}

	auto *attribute = reinterpret_cast<struct rtattr *>(reinterpret_cast<uint8_t *>(header) +
													  NLMSG_ALIGN(header->nlmsg_len));
	attribute->rta_type = type;
	attribute->rta_len = attribute_length;
	if (data != nullptr && length > 0) {
		std::memcpy(RTA_DATA(attribute), data, length);
	}
	header->nlmsg_len = new_length;
	return attribute;
}

struct rtattr *StartNested(struct nlmsghdr *header, int max_length, uint16_t type)
{
	return AddAttribute(header, max_length, type, nullptr, 0);
}

void EndNested(struct nlmsghdr *header, struct rtattr *nested)
{
	nested->rta_len = reinterpret_cast<uint8_t *>(header) + NLMSG_ALIGN(header->nlmsg_len) -
					  reinterpret_cast<uint8_t *>(nested);
}

const struct rtattr *FindAttribute(const void *data, int length, uint16_t type)
{
	auto *attribute = static_cast<const struct rtattr *>(data);
	while (RTA_OK(attribute, length)) {
		if ((attribute->rta_type & NLA_TYPE_MASK) == type) {
			return attribute;
		}
		attribute = RTA_NEXT(attribute, length);
	}
	return nullptr;
}

const struct rtattr *FindElementAttribute(const struct nlmsghdr *header, uint16_t type)
{
	int list_length = NLMSG_PAYLOAD(header, sizeof(struct nfgenmsg));
	auto *list = reinterpret_cast<const struct rtattr *>(
		reinterpret_cast<const uint8_t *>(NLMSG_DATA(header)) + NLMSG_ALIGN(sizeof(struct nfgenmsg)));
	auto *elements = FindAttribute(list, list_length, NFTA_SET_ELEM_LIST_ELEMENTS);
	if (elements == nullptr) {
		return nullptr;
	}

	int elements_length = RTA_PAYLOAD(elements);
	auto *element = FindAttribute(RTA_DATA(elements), elements_length, NFTA_LIST_ELEM);
	if (element == nullptr) {
		return nullptr;
	}

	int element_length = RTA_PAYLOAD(element);
	return FindAttribute(RTA_DATA(element), element_length, type);
}

uint64_t ReadBigEndian64(const struct rtattr *attribute)
{
	uint64_t value = 0;
	std::memcpy(&value, RTA_DATA(attribute), sizeof(value));
	return be64toh(value);
}

int BuildSetReply(void *reply, int reply_length, bool timeout_set)
{
	if (reply_length < kPayloadMax) {
		return -1;
	}
	std::memset(reply, 0, reply_length);
	auto *request = static_cast<NlmsgRequest *>(reply);
	request->header.nlmsg_len = NLMSG_LENGTH(sizeof(struct nfgenmsg));
	request->header.nlmsg_type = NFNL_SUBSYS_NFTABLES << 8 | NFT_MSG_NEWSET;
	uint32_t flags = htonl(timeout_set ? NFT_SET_TIMEOUT : 0);
	AddAttribute(&request->header, reply_length, NFTA_SET_FLAGS, &flags, sizeof(flags));
	return request->header.nlmsg_len;
}

int BuildElementReply(void *reply, int reply_length, uint64_t expiration_ms, bool has_expiration)
{
	if (reply_length < kPayloadMax) {
		return -1;
	}
	std::memset(reply, 0, reply_length);
	auto *request = static_cast<NlmsgRequest *>(reply);
	request->header.nlmsg_len = NLMSG_LENGTH(sizeof(struct nfgenmsg));
	request->header.nlmsg_type = NFNL_SUBSYS_NFTABLES << 8 | NFT_MSG_NEWSETELEM;
	auto *elements = StartNested(&request->header, reply_length,
								 NLA_F_NESTED | NFTA_SET_ELEM_LIST_ELEMENTS);
	auto *element = StartNested(&request->header, reply_length, NLA_F_NESTED | NFTA_LIST_ELEM);
	if (has_expiration) {
		uint64_t value = htobe64(expiration_ms);
		AddAttribute(&request->header, reply_length, NFTA_SET_ELEM_EXPIRATION, &value, sizeof(value));
	}
	EndNested(&request->header, element);
	EndNested(&request->header, elements);
	return request->header.nlmsg_len;
}

class FakeNftKernel {
public:
	bool timeout_set = true;
	bool exists = false;
	bool has_expiration = true;
	uint64_t expiration_ms = 0;
	uint64_t last_timeout_ms = 0;
	uint64_t last_expiration_ms = 0;
	int last_key_length = 0;
	uint16_t last_flags = 0;
	int get_set_count = 0;
	int get_element_count = 0;
	int new_element_count = 0;
	int delete_element_count = 0;

	int Request(const void *request_data, int request_length, void *reply, int reply_length)
	{
		std::lock_guard<std::mutex> guard(mutex_);
		int remaining = request_length;
		auto *header = static_cast<const struct nlmsghdr *>(request_data);
		while (NLMSG_OK(header, remaining)) {
			uint16_t message_type = header->nlmsg_type & 0xff;
			switch (message_type) {
			case NFT_MSG_GETSET:
				get_set_count++;
				return BuildSetReply(reply, reply_length, timeout_set);
			case NFT_MSG_GETSETELEM:
				get_element_count++;
				if (!exists) {
					errno = ENOENT;
					return -1;
				}
				return BuildElementReply(reply, reply_length, expiration_ms, has_expiration);
			case NFT_MSG_DELSETELEM:
				delete_element_count++;
				exists = false;
				break;
			case NFT_MSG_NEWSETELEM:
				ApplyNewElement(header);
				break;
			default:
				break;
			}
			header = NLMSG_NEXT(header, remaining);
		}
		return 0;
	}

private:
	void ApplyNewElement(const struct nlmsghdr *header)
	{
		new_element_count++;
		last_flags = header->nlmsg_flags;
		auto *timeout = FindElementAttribute(header, NFTA_SET_ELEM_TIMEOUT);
		auto *expiration = FindElementAttribute(header, NFTA_SET_ELEM_EXPIRATION);
		auto *key = FindElementAttribute(header, NFTA_SET_ELEM_KEY);
		last_timeout_ms = timeout == nullptr ? 0 : ReadBigEndian64(timeout);
		last_expiration_ms = expiration == nullptr ? 0 : ReadBigEndian64(expiration);
		last_key_length = 0;
		if (key != nullptr) {
			int key_length = RTA_PAYLOAD(key);
			auto *value = FindAttribute(RTA_DATA(key), key_length, NFTA_DATA_VALUE);
			if (value != nullptr) {
				last_key_length = RTA_PAYLOAD(value);
			}
		}
		exists = true;
		has_expiration = expiration != nullptr;
		expiration_ms = last_expiration_ms != 0 ? last_expiration_ms : last_timeout_ms;
	}

	std::mutex mutex_;
};

FakeNftKernel *active_kernel = nullptr;

int FakeRequest(const void *request, int request_length, void *reply, int reply_length)
{
	return active_kernel->Request(request, request_length, reply, reply_length);
}

class NftsetTest : public testing::Test {
protected:
	void SetUp() override
	{
		active_kernel = &kernel;
		nftset_set_request_callback_for_test(FakeRequest);
	}

	void TearDown() override
	{
		nftset_set_request_callback_for_test(nullptr);
		active_kernel = nullptr;
	}

	FakeNftKernel kernel;
	const unsigned char ipv4[4] = {192, 0, 2, 1};
	const unsigned char ipv6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
};

TEST(NftsetProtocol, ParsesActualRemainingExpiration)
{
	unsigned char reply[kPayloadMax] = {};
	int reply_length = BuildElementReply(reply, sizeof(reply), 30000, true);
	uint64_t expiration_ms = 0;
	int has_expiration = 0;
	EXPECT_EQ(nftset_parse_element_reply_for_test(reply, reply_length, &expiration_ms, &has_expiration), 1);
	EXPECT_EQ(expiration_ms, 30000U);
	EXPECT_EQ(has_expiration, 1);
}

TEST_F(NftsetTest, MissingTimedElementsAreAddedForIpv4AndIpv6)
{
	ASSERT_EQ(nftset_upsert_timed("inet", "filter", "policy4", ipv4, sizeof(ipv4), 300), 0);
	EXPECT_TRUE(kernel.exists);
	EXPECT_EQ(kernel.last_key_length, 4);
	EXPECT_EQ(kernel.last_timeout_ms, 300000U);
	EXPECT_EQ(kernel.last_expiration_ms, 300000U);

	kernel.exists = false;
	ASSERT_EQ(nftset_upsert_timed("inet", "filter", "policy6", ipv6, sizeof(ipv6), 300), 0);
	EXPECT_EQ(kernel.last_key_length, 16);
}

TEST_F(NftsetTest, ExistingTimedElementIsOnlyExtended)
{
	kernel.exists = true;
	kernel.expiration_ms = 30000;
	ASSERT_EQ(nftset_upsert_timed("inet", "filter", "policy4", ipv4, sizeof(ipv4), 300), 0);
	EXPECT_EQ(kernel.new_element_count, 1);
	EXPECT_EQ(kernel.expiration_ms, 300000U);
	EXPECT_EQ(kernel.delete_element_count, 0);
	EXPECT_NE(kernel.last_flags & NLM_F_CREATE, 0);
	EXPECT_EQ(kernel.last_flags & NLM_F_EXCL, 0);

	ASSERT_EQ(nftset_upsert_timed("inet", "filter", "policy4", ipv4, sizeof(ipv4), 30), 0);
	EXPECT_EQ(kernel.new_element_count, 1);
	EXPECT_EQ(kernel.expiration_ms, 300000U);
}

TEST_F(NftsetTest, SharedIpKeepsLatestLeaseRegardlessOfResponseOrder)
{
	ASSERT_EQ(nftset_upsert_timed("inet", "filter", "policy4", ipv4, sizeof(ipv4), 60), 0);
	ASSERT_EQ(nftset_upsert_timed("inet", "filter", "policy4", ipv4, sizeof(ipv4), 900), 0);
	ASSERT_EQ(nftset_upsert_timed("inet", "filter", "policy4", ipv4, sizeof(ipv4), 30), 0);
	EXPECT_EQ(kernel.expiration_ms, 900000U);
	EXPECT_EQ(kernel.delete_element_count, 0);
}

TEST_F(NftsetTest, DescendingCacheTtlDoesNotMoveAbsoluteExpiryForward)
{
	for (unsigned long timeout : {900UL, 600UL, 300UL, 30UL}) {
		ASSERT_EQ(nftset_upsert_timed("inet", "filter", "policy4", ipv4, sizeof(ipv4), timeout), 0);
	}
	EXPECT_EQ(kernel.expiration_ms, 900000U);
	EXPECT_EQ(kernel.new_element_count, 1);
}

TEST_F(NftsetTest, MissingElementSelfHealsAfterExternalFlush)
{
	ASSERT_EQ(nftset_upsert_timed("inet", "filter", "policy4", ipv4, sizeof(ipv4), 300), 0);
	kernel.exists = false;
	kernel.expiration_ms = 0;
	ASSERT_EQ(nftset_upsert_timed("inet", "filter", "policy4", ipv4, sizeof(ipv4), 270), 0);
	EXPECT_TRUE(kernel.exists);
	EXPECT_EQ(kernel.expiration_ms, 270000U);
	EXPECT_EQ(kernel.new_element_count, 2);
}

TEST_F(NftsetTest, PermanentSetKeepsExistingElementUntouched)
{
	kernel.timeout_set = false;
	kernel.exists = true;
	kernel.has_expiration = false;
	ASSERT_EQ(nftset_add("inet", "filter", "permanent", ipv4, sizeof(ipv4), 0), 0);
	EXPECT_EQ(kernel.new_element_count, 0);
	EXPECT_EQ(kernel.delete_element_count, 0);
}

TEST_F(NftsetTest, ConcurrentUpdatesKeepLongestLease)
{
	std::vector<unsigned long> timeouts = {2, 30, 7, 900, 60, 300, 1, 120};
	std::atomic<bool> start(false);
	std::vector<std::thread> threads;
	for (unsigned long timeout : timeouts) {
		threads.emplace_back([&, timeout]() {
			while (!start.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
			EXPECT_EQ(nftset_upsert_timed("inet", "filter", "policy4", ipv4, sizeof(ipv4), timeout), 0);
		});
	}
	start.store(true, std::memory_order_release);
	for (auto &thread : threads) {
		thread.join();
	}
	EXPECT_EQ(kernel.expiration_ms, 900000U);
	EXPECT_EQ(kernel.delete_element_count, 0);
}

TEST(NftsetDnsPath, CacheAndUpstreamUseDifferentEffectiveTtl)
{
	EXPECT_EQ(dns_server_get_nftset_timeout_for_test(0, 3, 600), 1800UL);
	EXPECT_EQ(dns_server_get_nftset_timeout_for_test(1, 100, 600), 300UL);
	EXPECT_EQ(dns_server_get_nftset_timeout_for_test(1, 0, 600), 0UL);
	EXPECT_EQ(dns_server_get_nftset_timeout_for_test(1, -1, 600), 0UL);

	uint64_t expected = std::min<uint64_t>(static_cast<uint64_t>(ULONG_MAX),
										 static_cast<uint64_t>(INT_MAX) * 3U);
	EXPECT_EQ(dns_server_get_nftset_timeout_for_test(0, 0, INT_MAX), static_cast<unsigned long>(expected));
}

TEST(NftsetDnsPath, ValidVisitedCacheRefreshesOnlyTimedNftset)
{
	EXPECT_TRUE(dns_server_cache_should_update_nftset_for_test(100, 1, 1));
	EXPECT_FALSE(dns_server_cache_should_update_nftset_for_test(100, 1, 0));
	EXPECT_TRUE(dns_server_cache_should_update_nftset_for_test(0, 1, 0));
	EXPECT_TRUE(dns_server_cache_should_update_nftset_for_test(100, 0, 0));
}

TEST(NftsetDnsPath, HttpsHintsCanReachPacketWalkerWithoutSelectedIp)
{
	EXPECT_TRUE(dns_server_should_walk_ipset_nftset_for_test(DNS_T_HTTPS, 0));
	EXPECT_FALSE(dns_server_should_walk_ipset_nftset_for_test(DNS_T_A, 0));
	EXPECT_TRUE(dns_server_should_walk_ipset_nftset_for_test(DNS_T_A, 1));
}

} // namespace
