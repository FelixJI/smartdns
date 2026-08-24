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

#ifndef _NFTSET_H
#define _NFTSET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int nftset_add(const char *familyname, const char *tablename, const char *setname, const unsigned char addr[],
			   int addr_len, unsigned long timeout);

int nftset_upsert_timed(const char *familyname, const char *tablename, const char *setname,
						const unsigned char addr[], int addr_len, unsigned long desired_timeout);

int nftset_del(const char *familyname, const char *tablename, const char *setname, const unsigned char addr[],
			   int addr_len);

#ifdef TEST
typedef int (*nftset_request_callback_t)(const void *request, int request_len, void *reply, int reply_len);

void nftset_set_request_callback_for_test(nftset_request_callback_t callback);

int nftset_parse_element_reply_for_test(const void *reply, int reply_len, uint64_t *expiration_ms,
									int *has_expiration);
#endif

#ifdef __cplusplus
}
#endif

#endif // !_NFTSET_H
