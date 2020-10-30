/*
* Copyright 2020 Intel Corporation.
*
* This software and the related documents are Intel copyrighted materials,
* and your use of them is governed by the express license under which they
* were provided to you ("License").
* Unless the License provides otherwise, you may not use, modify, copy,
* publish, distribute, disclose or transmit this software or the related
* documents without Intel's prior written permission.
*
* This software and the related documents are provided as is, with no
* express or implied warranties, other than those that are expressly stated
* in the License.
*
*/

extern "C"
{
#include "../include/st_pack.h"
}
#include <stdio.h>
#include <unistd.h>

typedef int error;

struct pair
{
	void (*Pack)(st_rfc4175_422_10_pg2_t *pg, uint16_t cb00, uint16_t y00, uint16_t cr00,
				 uint16_t y01);
	void (*UnPack)(st_rfc4175_422_10_pg2_t const *pg, uint16_t *cb00, uint16_t *y00, uint16_t *cr00,
				   uint16_t *y01);
	uint16_t (*ntohs)(uint16_t);
};

static uint16_t
NoSwap(uint16_t v)
{
	return v;
}
static uint16_t
Swap(uint16_t v)
{
	return (v << 8) | (v >> 8);
}

static void
B()
{ // short name for gdb breakpoint
}

static error
Log(uint16_t V1, uint16_t V2, error E)
{
	if (V1 == V2)
		return 0;
	char T[0x100];
	write(2, T, snprintf(T, sizeof(T), "%d: %x != %x\n", E, V1, V2));
	B();
	return E;
}

static error
Mixer(uint16_t const B, uint16_t const Y0, uint16_t const R, uint16_t const Y1, pair const &P)
{
	st_rfc4175_422_10_pg2_t tmp;
	P.Pack(&tmp, B, Y0, R, Y1);
	uint16_t BB, YY0, RR, YY1;
	P.UnPack(&tmp, &BB, &YY0, &RR, &YY1);
	if (error e = Log(B, BB, 1))
		return e;
	if (error e = Log(Y0, YY0, 2))
		return e;
	if (error e = Log(R, RR, 3))
		return e;
	if (error e = Log(Y1, YY1, 4))
		return e;
	return 0;
}

static error
Test()
{
	static const pair Pair[] = {
		{ Pack_422be10_PG2be, Unpack_PG2be_422be10, Swap },
		{ Pack_422le10_PG2be, Unpack_PG2be_422le10, NoSwap },
		{ Pack_422le10_PG2le, Unpack_PG2le_422le10, NoSwap },
		{ Pack_422be10_PG2le, Unpack_PG2le_422be10, Swap },
	};
	for (auto &P : Pair)
	{
		for (uint16_t Bit = 1 << 9; Bit; Bit >>= 1)
		{
			uint16_t const Value = P.ntohs(Bit);
			if (int rc = Mixer(0, 0, 0, Value, P))
				return rc + 40;
			if (int rc = Mixer(0, 0, Value, 0, P))
				return rc + 30;
			if (int rc = Mixer(0, Value, 0, 0, P))
				return rc + 20;
			if (int rc = Mixer(Value, 0, 0, 0, P))
				return rc + 10;
		}
	}
	return 0;
}

error
main()
{
	if (error Rc = Test())
	{
		char T[0x100];
		write(1, T, snprintf(T, sizeof(T), "FAIL %d\n", Rc));
		return Rc;
	}
	write(1, "PASS\n", 5);
	return 0;
}
