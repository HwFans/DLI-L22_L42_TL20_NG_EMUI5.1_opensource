/*
** =============================================================================
** Copyright (c) 2016  Texas Instruments Inc.
**
** This program is free software; you can redistribute it and/or modify it under
** the terms of the GNU General Public License as published by the Free Software
** Foundation; version 2.
**
** This program is distributed in the hope that it will be useful, but WITHOUT
** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
** FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License along with
** this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
** Street, Fifth Floor, Boston, MA 02110-1301, USA.
**
** File:
**     tas2560-core.h
**
** Description:
**     header file for tas2560-core.c
**
** =============================================================================
*/

#ifndef _TAS2560_CORE_H
#define _TAS2560_CORE_H

#include "tas2560.h"

int tas2560_set_bit_rate(struct tas2560_priv *pTAS2560, unsigned int nBitRate);
int tas2560_get_bit_rate(struct tas2560_priv *pTAS2560);
int tas2560_set_ASI_fmt(struct tas2560_priv *pTAS2560, unsigned int fmt);
int tas2560_setLoad(struct tas2560_priv *pTAS2560, int load);
int tas2560_getLoad(struct tas2560_priv *pTAS2560);
int tas2560_get_volume(struct tas2560_priv *pTAS2560);
int tas2560_set_volume(struct tas2560_priv *pTAS2560, int volume);
int tas2560_set_output_channel(struct tas2560_priv *pTAS2560, unsigned int channel);
int tas2560_set_mute(struct tas2560_priv *pTAS2560, unsigned int mute);
int tas2560_set_dsp_mode(struct tas2560_priv *pTAS2560, unsigned value);
int tas2560_set_offset(struct tas2560_priv *pTAS2560, unsigned int offset);
int tas2560_set_samplerate(struct tas2560_priv *pTAS2560, unsigned int nSamplingRate);
int tas2560_set_pll(struct tas2560_priv *pTAS2560, int pll_clkin);
int tas2560_set_pll_clkin(struct tas2560_priv *pTAS2560, int clk_id, unsigned int freq);
int tas2560_parse_dt(struct device *dev, struct tas2560_priv *pTAS2560);
void tas2560_hw_reset(struct tas2560_priv *pTAS2560);
void tas2560_sw_shutdown(struct tas2560_priv *pTAS2560, int sw_shutdown);
void tas2560_enable(struct tas2560_priv *pTAS2560, bool bEnable);
void tas2560_hw_setup(struct tas2560_priv *pTAS2560);
#endif /* _TAS2560_CORE_H */
