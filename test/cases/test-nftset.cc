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

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <endian.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

namespace {

const struct rtattr *FindAttribute(const void *data, int length, unsigned short type)
{
	auto *attr = static_cast<const struct rtattr *>(data);
	while (RTA_OK(attr, length)) {
		if ((attr->rta_type & NLA_TYPE_MASK) == type) {
			return attr;
		}
		attr = RTA_NEXT(attr, length);
	}
	return nullptr;
}

const struct rtattr *FindElementAttribute(const unsigned char *message, int message_length, unsigned short type)
{
	auto *header = reinterpret_cast<const struct nlmsghdr *>(message);
	if (!NLMSG_OK(header, message_length)) {
		return nullptr;
	}

	int list_length = NLMSG_PAYLOAD(header, sizeof(struct nfgenmsg));
	auto *list = reinterpret_cast<const struct rtattr *>(
		reinterpret_cast<const unsigned char *>(NLMSG_DATA(header)) + NLMSG_ALIGN(sizeof(struct nfgenmsg)));
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

uint64_t ReadBigEndian64(const struct rtattr *attr)
{
	uint64_t value = 0;
	memcpy(&value, RTA_DATA(attr), sizeof(value));
	return be64toh(value);
}

} // namespace

TEST(Nftset, TimedExistingElementIsUpdated)
{
	EXPECT_FALSE(nftset_should_skip_existing_for_test(1, 160));
	EXPECT_TRUE(nftset_should_skip_existing_for_test(1, 0));
}

TEST(Nftset, TimedElementCarriesExpiration)
{
	unsigned char message[2048] = {};
	int message_length = nftset_build_add_element_for_test(160, message, sizeof(message));
	ASSERT_GT(message_length, 0);

	auto *timeout = FindElementAttribute(message, message_length, NFTA_SET_ELEM_TIMEOUT);
	auto *expiration = FindElementAttribute(message, message_length, NFTA_SET_ELEM_EXPIRATION);
	ASSERT_NE(timeout, nullptr);
	ASSERT_NE(expiration, nullptr);
	EXPECT_EQ(ReadBigEndian64(timeout), 160000U);
	EXPECT_EQ(ReadBigEndian64(expiration), 160000U);
}

TEST(Nftset, ValidCacheHitRefreshesTimedSet)
{
	EXPECT_TRUE(dns_server_cache_should_update_ipset_nftset_for_test(100, 1, 1));
	EXPECT_FALSE(dns_server_cache_should_update_ipset_nftset_for_test(100, 1, 0));
}

TEST(Nftset, TimeoutUsesReplyTtlPlusGrace)
{
	EXPECT_EQ(dns_server_get_nftset_timeout_value_for_test(100, 600, 60), 160);
	EXPECT_EQ(dns_server_get_nftset_timeout_value_for_test(0, 100, 60), 160);
	EXPECT_EQ(dns_server_get_nftset_timeout_value_for_test(0, 0, 60), 120);
}
