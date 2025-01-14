/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <nrf_errno.h>
#include "slm_util.h"
#include "slm_at_host.h"

LOG_MODULE_REGISTER(slm_util, CONFIG_SLM_LOG_LEVEL);

static bool cmd_name_has_lower(const char *cmd)
{
	for (size_t i = 0; cmd[i] != '\0'; ++i) {
		const char c = cmd[i];

		/* Set/test command names are delimited by '=', read by '?'. */
		if (c == '=' || c == '?') {
			break;
		} else if (islower(c)) {
			LOG_ERR("FIX ME: AT command \"%s\" must be all-uppercase.", cmd);
			return true;
		}
	}
	return false;
}

int slm_util_at_printf(const char *fmt, ...)
{
	int ret;
	char buf[128];
	va_list args;

	va_start(args, fmt);
	ret = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (ret >= sizeof(buf)) {
		LOG_ERR("AT command \"%.16s...\" would get truncated from %u to %u bytes. "
			"The buffer needs to be made bigger.", buf, ret, sizeof(buf) - 1);
		return -E2BIG;
	}

	/* AT command interception is case-sensitive. */
	if (cmd_name_has_lower(buf)) {
		return -EINVAL;
	}

	/* Even though we are not interested in the AT response, it must fit
	 * into the provided buffer so that the response code is returned.
	 */
	ret = nrf_modem_at_cmd(buf, sizeof(buf), "%s", buf);
	if (ret == -NRF_E2BIG) {
		/* Unlikely, but in that case the response code most likely didn't
		 * make it into the buffer, so searching for it would be fruitless.
		 */
		LOG_ERR("AT response to \"%s\" didn't fit into %u bytes. "
			"The buffer needs to be made bigger.", fmt, sizeof(buf) - 1);
	}
	return ret;
}

int slm_util_at_scanf(const char *cmd, const char *fmt, ...)
{
	char buf[128];
	va_list args;
	int ret;

	/* AT command interception is case-sensitive. */
	if (cmd_name_has_lower(cmd)) {
		return -EINVAL;
	}

	ret = nrf_modem_at_cmd(buf, sizeof(buf), "%s", cmd);
	if (ret == -NRF_E2BIG) {
		LOG_ERR("AT response to \"%s\" truncated to %u bytes. "
			"The buffer needs to be made bigger.", cmd, sizeof(buf) - 1);
		buf[sizeof(buf) - 1] = '\0';
	} else if (ret < 0) {
		return ret;
	}

	va_start(args, fmt);
	ret = vsscanf(buf, fmt, args);
	va_end(args);

	if (ret == 0) {
		ret = -NRF_EBADMSG;
	}
	return ret;
}

int slm_util_at_cmd_fwd_from_cb(char *buf, size_t len, char *at_cmd)
{
/* This gets called by interception callbacks invoked by nrf_modem_at_cmd().
 * Here nrf_modem_at_cmd() must be bypassed because it would otherwise call
 * the same interception callbacks and infinite recursion would happen.
 */
#undef nrf_modem_at_printf
	const int ret = nrf_modem_at_printf("%s", at_cmd);

/* Restore the trap macro after the only place where this function is
 * allowed to be used so that other code does not accidentally use it.
 */
#define nrf_modem_at_printf nrf_modem_at_scanf

	if (ret < 0) {
		LOG_ERR("Forwarding of \"%s\" failed (%d).", at_cmd, ret);
		return ret;
	}

	const int err = nrf_modem_at_err_type(ret);

	if (!ret || err == NRF_MODEM_AT_ERROR) {
		snprintf(buf, len, "%s\r\n", ret ? "ERROR" : "OK");
	} else {
		snprintf(buf, len, "+CM%c ERROR: %d\r\n",
			err == NRF_MODEM_AT_CME_ERROR ? 'E' : 'S', nrf_modem_at_err(ret));
	}
	return ret;
}

bool slm_util_casecmp(const char *str1, const char *str2)
{
	int str2_len = strlen(str2);

	if (strlen(str1) != str2_len) {
		return false;
	}

	for (int i = 0; i < str2_len; i++) {
		if (toupper((int)*(str1 + i)) != toupper((int)*(str2 + i))) {
			return false;
		}
	}

	return true;
}

bool slm_util_hexstr_check(const uint8_t *data, uint16_t data_len)
{
	for (int i = 0; i < data_len; i++) {
		char ch = *(data + i);

		if ((ch < '0' || ch > '9') &&
		    (ch < 'A' || ch > 'F') &&
		    (ch < 'a' || ch > 'f')) {
			return false;
		}
	}

	return true;
}

int slm_util_htoa(const uint8_t *hex, uint16_t hex_len, char *ascii, uint16_t ascii_len)
{
	if (hex == NULL || ascii == NULL) {
		return -EINVAL;
	}
	if (ascii_len < (hex_len * 2)) {
		return -EINVAL;
	}

	for (int i = 0; i < hex_len; i++) {
		sprintf(ascii + (i * 2), "%02X", *(hex + i));
	}

	return (hex_len * 2);
}

int slm_util_atoh(const char *ascii, uint16_t ascii_len, uint8_t *hex, uint16_t hex_len)
{
	char hex_str[3];

	if (hex == NULL || ascii == NULL) {
		return -EINVAL;
	}
	if ((ascii_len % 2) > 0) {
		return -EINVAL;
	}
	if (ascii_len > (hex_len * 2)) {
		return -EINVAL;
	}
	if (!slm_util_hexstr_check(ascii, ascii_len)) {
		return -EINVAL;
	}

	hex_str[2] = '\0';
	for (int i = 0; (i * 2) < ascii_len; i++) {
		strncpy(&hex_str[0], ascii + (i * 2), 2);
		*(hex + i) = (uint8_t)strtoul(hex_str, NULL, 16);
	}

	return (ascii_len / 2);
}

int util_string_get(const struct at_param_list *list, size_t index, char *value, size_t *len)
{
	int ret;
	size_t size = *len;

	/* at_params_string_get calls "memcpy" instead of "strcpy" */
	ret = at_params_string_get(list, index, value, len);
	if (ret) {
		return ret;
	}
	if (*len < size) {
		*(value + *len) = '\0';
		return 0;
	}

	return -ENOMEM;
}

int util_string_to_float_get(const struct at_param_list *list, size_t index, float *value)
{
	int ret;
	char str[32];
	size_t len = sizeof(str);

	ret = util_string_get(list, index, str, &len);
	if (ret) {
		return ret;
	}

	*value = strtof(str, NULL);

	return 0;
}

int util_string_to_double_get(const struct at_param_list *list, size_t index, double *value)
{
	int ret;
	char str[32];
	size_t len = sizeof(str);

	ret = util_string_get(list, index, str, &len);
	if (ret) {
		return ret;
	}

	*value = strtod(str, NULL);

	return 0;
}

void util_get_ip_addr(int cid, char *addr4, char *addr6)
{
	int ret;
	char cmd[128];
	char tmp[sizeof(struct in6_addr)];
	char addr1[NET_IPV6_ADDR_LEN] = { 0 };
	char addr2[NET_IPV6_ADDR_LEN] = { 0 };

	sprintf(cmd, "AT+CGPADDR=%d", cid);
	/** parse +CGPADDR: <cid>,<PDP_addr_1>,<PDP_addr_2>
	 * PDN type "IP": PDP_addr_1 is <IPv4>, max 16(INET_ADDRSTRLEN), '.' and digits
	 * PDN type "IPV6": PDP_addr_1 is <IPv6>, max 46(INET6_ADDRSTRLEN),':', digits, 'A'~'F'
	 * PDN type "IPV4V6": <IPv4>,<IPv6> or <IPV4> or <IPv6>
	 */
	ret = slm_util_at_scanf(cmd, "+CGPADDR: %*d,\"%46[.:0-9A-F]\",\"%46[:0-9A-F]\"",
				 addr1, addr2);
	if (ret <= 0) {
		return;
	}
	if (addr4 != NULL && inet_pton(AF_INET, addr1, tmp) == 1) {
		strcpy(addr4, addr1);
	} else if (addr6 != NULL && inet_pton(AF_INET6, addr1, tmp) == 1) {
		strcpy(addr6, addr1);
		return;
	}
	/* parse second IP string, IPv6 only */
	if (addr6 == NULL) {
		return;
	}
	if (ret > 1 && inet_pton(AF_INET6, addr2, tmp) == 1) {
		strcpy(addr6, addr2);
	}
}

int util_str_to_int(const char *str_buf, int base, int *output)
{
	int temp;
	char *end_ptr = NULL;

	errno = 0;
	temp = strtol(str_buf, &end_ptr, base);

	if (end_ptr == str_buf || *end_ptr != '\0' ||
	    ((temp == LONG_MAX || temp == LONG_MIN) && errno == ERANGE)) {
		return -ENODATA;
	}

	*output = temp;
	return 0;
}

#define PORT_MAX_SIZE    5 /* 0xFFFF = 65535 */
#define PDN_ID_MAX_SIZE  2 /* 0..10 */

int util_resolve_host(int cid, const char *host, uint16_t port, int family,
	LOG_INSTANCE_PTR_DECLARE(log_inst), struct sockaddr *sa)
{
	int err;
	char service[PORT_MAX_SIZE + PDN_ID_MAX_SIZE + 2];
	struct addrinfo *ai = NULL;
	struct addrinfo hints = {
		.ai_flags  = AI_NUMERICSERV | AI_PDNSERV,
		.ai_family = family
	};

	if (sa == NULL) {
		return DNS_EAI_AGAIN;
	}

	/* "service" shall be formatted as follows: "port:pdn_id" */
	snprintf(service, sizeof(service), "%hu:%d", port, cid);
	err = getaddrinfo(host, service, &hints, &ai);
	if (!err) {
		*sa = *(ai->ai_addr);
		freeaddrinfo(ai);

		if (sa->sa_family != AF_INET && sa->sa_family != AF_INET6) {
			err = DNS_EAI_ADDRFAMILY;
		}
	}

	if (err) {
		const char *errstr;

		if (err == DNS_EAI_SYSTEM) {
			errstr = strerror(errno);
			err = errno;
		} else {
			errstr = gai_strerror(err);
		}
		LOG_INST_ERR(log_inst, "getaddrinfo() error (%d): %s", err, errstr);
	}
	return err;
}
