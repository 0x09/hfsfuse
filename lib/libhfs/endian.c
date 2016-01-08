/*-
 * Copyright (c) 2005, 2007 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Yevgeny Binder and Dieter Baron.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>
#include <stdint.h>
#include "byteorder.h"

uint16_t be16tohp(void** inout_ptr)
{
	uint16_t	result;
	
	if(inout_ptr == NULL)
		return 0;
		
	memcpy(&result, *inout_ptr, sizeof(result));
	*inout_ptr = (char *)*inout_ptr + sizeof(result);
	
	return be16toh(result);
}

uint32_t be32tohp(void** inout_ptr)
{
	uint32_t	result;
	
	if(inout_ptr == NULL)
		return 0;

	memcpy(&result, *inout_ptr, sizeof(result));
	*inout_ptr = (char *)*inout_ptr + sizeof(result);
	return be32toh(result);
}

uint64_t be64tohp(void** inout_ptr)
{
	uint64_t	result;
	
	if(inout_ptr == NULL)
		return 0;

	memcpy(&result, *inout_ptr, sizeof(result));
	*inout_ptr = (char *)*inout_ptr + sizeof(result);
	return be64toh(result);
}
