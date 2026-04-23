#ifndef CHAIN_MID_HLSLI
#define CHAIN_MID_HLSLI

#include "chain_leaf.hlsli"

uint chainMid(uint x) { return chainLeaf(x) * 2u; }

#endif
