/***********************************************************************
Copyright (c) 2006-2011, Skype Limited. All rights reserved.
Redistribution and use in source and binary forms, with or without
modification, (subject to the limitations in the disclaimer below)
are permitted provided that the following conditions are met:
- Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
- Neither the name of Skype Limited, nor the names of specific
contributors, may be used to endorse or promote products derived from
this software without specific prior written permission.
NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED
BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
CONTRIBUTORS ''AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
***********************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "main_FLP.h"
#include "tuning_parameters.h"

/* Low Bitrate Redundancy (LBRR) encoding. Reuse all parameters but encode with lower bitrate           */
static inline void silk_LBRR_encode_FLP(
    silk_encoder_state_FLP          *psEnc,             /* I/O  Encoder state FLP                       */
    silk_encoder_control_FLP        *psEncCtrl,         /* I/O  Encoder control FLP                     */
    const silk_float                 xfw[],              /* I    Input signal                            */
    opus_int                         condCoding         /* I    The type of conditional coding used so far for this frame */
);

void silk_encode_do_VAD_FLP(
    silk_encoder_state_FLP          *psEnc              /* I/O  Encoder state FLP                       */
)
{
    /****************************/
    /* Voice Activity Detection */
    /****************************/
TIC(VAD)
    silk_VAD_GetSA_Q8( &psEnc->sCmn, psEnc->sCmn.inputBuf + 1 );
TOC(VAD)

    /**************************************************/
    /* Convert speech activity into VAD and DTX flags */
    /**************************************************/
    if( psEnc->sCmn.nFramesEncoded == 0 ) {
        psEnc->sCmn.inDTX = psEnc->sCmn.useDTX;
    }
    if( psEnc->sCmn.speech_activity_Q8 < SILK_FIX_CONST( SPEECH_ACTIVITY_DTX_THRES, 8 ) ) {
        psEnc->sCmn.indices.signalType = TYPE_NO_VOICE_ACTIVITY;
        psEnc->sCmn.noSpeechCounter++;
        if( psEnc->sCmn.noSpeechCounter < NB_SPEECH_FRAMES_BEFORE_DTX ) {
            psEnc->sCmn.inDTX = 0;
        } else if( psEnc->sCmn.noSpeechCounter > MAX_CONSECUTIVE_DTX + NB_SPEECH_FRAMES_BEFORE_DTX ) {
            psEnc->sCmn.noSpeechCounter = NB_SPEECH_FRAMES_BEFORE_DTX;
            psEnc->sCmn.inDTX           = 0;
        }
        psEnc->sCmn.VAD_flags[ psEnc->sCmn.nFramesEncoded ] = 0;
    } else {
        psEnc->sCmn.noSpeechCounter    = 0;
        psEnc->sCmn.inDTX              = 0;
        psEnc->sCmn.indices.signalType = TYPE_UNVOICED;
        psEnc->sCmn.VAD_flags[ psEnc->sCmn.nFramesEncoded ] = 1;
    }
}

/****************/
/* Encode frame */
/****************/
opus_int silk_encode_frame_FLP(
    silk_encoder_state_FLP          *psEnc,             /* I/O  Encoder state FLP                       */
    opus_int32                       *pnBytesOut,        /*   O  Number of payload bytes                 */
    ec_enc                          *psRangeEnc,        /* I/O  compressor data structure               */
    opus_int                         condCoding,        /* I    The type of conditional coding to use   */
    opus_int                         maxBits,           /* I    If > 0: maximum number of output bits   */
    opus_int                         useCBR             /* I    Flag to force constant-bitrate operation */
)
{
    silk_encoder_control_FLP sEncCtrl;
    opus_int     i, iter, maxIter, found_upper, found_lower, ret = 0;
    silk_float   *x_frame, *res_pitch_frame;
    silk_float   xfw[ MAX_FRAME_LENGTH ];
    silk_float   res_pitch[ 2 * MAX_FRAME_LENGTH + LA_PITCH_MAX ];
    ec_enc       sRangeEnc_copy, sRangeEnc_copy2;
    silk_nsq_state sNSQ_copy, sNSQ_copy2;
    opus_int32   seed_copy, nBits, nBits_lower, nBits_upper, gainMult_lower, gainMult_upper;
    opus_int16   gainMult_Q8;
    opus_int16   ec_prevLagIndex_copy;
    opus_int     ec_prevSignalType_copy;
    opus_int8    LastGainIndex_copy2;
    opus_int32   pGains_Q16[ MAX_NB_SUBFR ];
    opus_uint8   ec_buf_copy[ 1275 ];

TIC(ENCODE_FRAME)

    /* This is totally unnecessary but many compilers (including gcc) are too dumb
       to realise it */
    LastGainIndex_copy2 = nBits_lower = nBits_upper = gainMult_lower = gainMult_upper = 0;

    psEnc->sCmn.indices.Seed = psEnc->sCmn.frameCounter++ & 3;

    /**************************************************************/
    /* Setup Input Pointers, and insert frame in input buffer    */
    /*************************************************************/
    /* pointers aligned with start of frame to encode */
    x_frame         = psEnc->x_buf + psEnc->sCmn.ltp_mem_length;    /* start of frame to encode */
    res_pitch_frame = res_pitch    + psEnc->sCmn.ltp_mem_length;    /* start of pitch LPC residual frame */

    /***************************************/
    /* Ensure smooth bandwidth transitions */
    /***************************************/
    silk_LP_variable_cutoff( &psEnc->sCmn.sLP, psEnc->sCmn.inputBuf + 1, psEnc->sCmn.frame_length );

    /*******************************************/
    /* Copy new frame to front of input buffer */
    /*******************************************/
    silk_short2float_array( x_frame + LA_SHAPE_MS * psEnc->sCmn.fs_kHz, psEnc->sCmn.inputBuf + 1, psEnc->sCmn.frame_length );

    /* Add tiny signal to avoid high CPU load from denormalized floating point numbers */
    for( i = 0; i < 8; i++ ) {
        x_frame[ LA_SHAPE_MS * psEnc->sCmn.fs_kHz + i * ( psEnc->sCmn.frame_length >> 3 ) ] += ( 1 - ( i & 2 ) ) * 1e-6f;
    }

    /*****************************************/
    /* Find pitch lags, initial LPC analysis */
    /*****************************************/
TIC(FIND_PITCH)
    silk_find_pitch_lags_FLP( psEnc, &sEncCtrl, res_pitch, x_frame );
TOC(FIND_PITCH)

    /************************/
    /* Noise shape analysis */
    /************************/
TIC(NOISE_SHAPE_ANALYSIS)
    silk_noise_shape_analysis_FLP( psEnc, &sEncCtrl, res_pitch_frame, x_frame );
TOC(NOISE_SHAPE_ANALYSIS)

    /***************************************************/
    /* Find linear prediction coefficients (LPC + LTP) */
    /***************************************************/
TIC(FIND_PRED_COEF)
    silk_find_pred_coefs_FLP( psEnc, &sEncCtrl, res_pitch, x_frame, condCoding );
TOC(FIND_PRED_COEF)

    /****************************************/
    /* Process gains                        */
    /****************************************/
TIC(PROCESS_GAINS)
    silk_process_gains_FLP( psEnc, &sEncCtrl, condCoding );
TOC(PROCESS_GAINS)

    /*****************************************/
    /* Prefiltering for noise shaper         */
    /*****************************************/
TIC(PREFILTER)
    silk_prefilter_FLP( psEnc, &sEncCtrl, xfw, x_frame );
TOC(PREFILTER)

    /****************************************/
    /* Low Bitrate Redundant Encoding       */
    /****************************************/
TIC(LBRR)
    silk_LBRR_encode_FLP( psEnc, &sEncCtrl, xfw, condCoding );
TOC(LBRR)

    if ( psEnc->sCmn.prefillFlag )
    {
TIC(NSQ)
        silk_NSQ_wrapper_FLP( psEnc, &sEncCtrl, &psEnc->sCmn.indices, &psEnc->sCmn.sNSQ, psEnc->sCmn.pulses, xfw );
TOC(NSQ)
    } else {
        /* Loop over quantizer and entroy coding to control bitrate */
        maxIter = 5;
        gainMult_Q8 = SILK_FIX_CONST( 1, 8 );
        found_lower = 0;
        found_upper = 0;
        for( iter = 0; ; iter++ ) {
            /* Copy part of the input state */
            silk_memcpy( &sRangeEnc_copy, psRangeEnc, sizeof( ec_enc ) );
            silk_memcpy( &sNSQ_copy, &psEnc->sCmn.sNSQ, sizeof( silk_nsq_state ) );
            seed_copy = psEnc->sCmn.indices.Seed;
            ec_prevLagIndex_copy = psEnc->sCmn.ec_prevLagIndex;
            ec_prevSignalType_copy = psEnc->sCmn.ec_prevSignalType;

            /*****************************************/
            /* Noise shaping quantization            */
            /*****************************************/
TIC(NSQ)
            silk_NSQ_wrapper_FLP( psEnc, &sEncCtrl, &psEnc->sCmn.indices, &psEnc->sCmn.sNSQ, psEnc->sCmn.pulses, xfw );
TOC(NSQ)

            /****************************************/
            /* Encode Parameters                    */
            /****************************************/
TIC(ENCODE_PARAMS)
            silk_encode_indices( &psEnc->sCmn, psRangeEnc, psEnc->sCmn.nFramesEncoded, 0, condCoding );
TOC(ENCODE_PARAMS)

            /****************************************/
            /* Encode Excitation Signal             */
            /****************************************/
TIC(ENCODE_PULSES)
            silk_encode_pulses( psRangeEnc, psEnc->sCmn.indices.signalType, psEnc->sCmn.indices.quantOffsetType,
                  psEnc->sCmn.pulses, psEnc->sCmn.frame_length );
TOC(ENCODE_PULSES)

            nBits = ec_tell( psRangeEnc );

            if( useCBR == 0 && iter == 0 && nBits <= maxBits ) {
                break;
            }

            if( iter == maxIter ) {
                if( nBits > maxBits && found_lower ) {
                    /* Restore output state from earlier iteration that did meet the bitrate budget */
                   silk_memcpy( psRangeEnc, &sRangeEnc_copy2, sizeof( ec_enc ) );
                   silk_assert( sRangeEnc_copy2.offs<=1275 );
                   silk_memcpy( psRangeEnc->buf, ec_buf_copy, sRangeEnc_copy2.offs );
                   silk_memcpy( &psEnc->sCmn.sNSQ, &sNSQ_copy2, sizeof( silk_nsq_state ) );
                   psEnc->sShape.LastGainIndex = LastGainIndex_copy2;
                }
                break;
            }

            if( nBits > maxBits ) {
                found_upper = 1;
                nBits_upper = nBits;
                gainMult_upper = gainMult_Q8;
                if( found_lower == 0 && iter >= 3 ) {
                    /* Adjust the quantizer's rate/distortion tradeoff */
                    sEncCtrl.Lambda *= 1.5f;
                }
            } else if( nBits < maxBits - 5 ) {
                found_lower = 1;
                nBits_lower = nBits;
                gainMult_lower = gainMult_Q8;
                /* Copy part of the output state */
                silk_memcpy( &sRangeEnc_copy2, psRangeEnc, sizeof( ec_enc ) );
                silk_assert( psRangeEnc->offs<=1275 );
                silk_memcpy( ec_buf_copy, psRangeEnc->buf, psRangeEnc->offs );
                silk_memcpy( &sNSQ_copy2, &psEnc->sCmn.sNSQ, sizeof( silk_nsq_state ) );
                LastGainIndex_copy2 = psEnc->sShape.LastGainIndex;
            } else {
                /* Within 5 bits of budget: close enough */
                break;
            }

            if( ( found_lower & found_upper ) == 0 ) {
                /* Adjust gain according to high-rate rate/distortion curve */
                opus_int32 gain_factor_Q16;
                gain_factor_Q16 = silk_log2lin( silk_LSHIFT( nBits - maxBits, 7 ) / psEnc->sCmn.frame_length + SILK_FIX_CONST( 16, 7 ) );
                gain_factor_Q16 = silk_min_32(gain_factor_Q16, SILK_FIX_CONST( 2, 16 ) );
                if( nBits > maxBits ) {
                    gain_factor_Q16 = silk_max_32( gain_factor_Q16, SILK_FIX_CONST( 1.3, 16 ) );
                }
                gainMult_Q8 = silk_SMULWB( gain_factor_Q16, gainMult_Q8 );
            } else {
                /* Adjust gain by interpolating */
                gainMult_Q8 = gainMult_lower + ( ( gainMult_upper - gainMult_lower ) * ( maxBits - nBits_lower ) ) / ( nBits_upper - nBits_lower );
                /* New gain multplier must be between 25% and 75% of old range (note that gainMult_upper < gainMult_lower) */
                if( gainMult_Q8 > gainMult_lower + silk_RSHIFT32( gainMult_upper - gainMult_lower, 2 ) ) {
                    gainMult_Q8 = gainMult_lower + silk_RSHIFT32( gainMult_upper - gainMult_lower, 2 );
                } else
                    if( gainMult_Q8 < gainMult_upper - silk_RSHIFT32( gainMult_upper - gainMult_lower, 2 ) ) {
                        gainMult_Q8 = gainMult_upper - silk_RSHIFT32( gainMult_upper - gainMult_lower, 2 );
                    }
            }

            for( i = 0; i < psEnc->sCmn.nb_subfr; i++ ) {
                pGains_Q16[ i ] = silk_LSHIFT_SAT32( silk_SMULWB( sEncCtrl.GainsUnq_Q16[ i ], gainMult_Q8 ), 8 );
            }
            psEnc->sShape.LastGainIndex = sEncCtrl.lastGainIndexPrev;

            /* Noise shaping quantization */
            silk_gains_quant( psEnc->sCmn.indices.GainsIndices, pGains_Q16,
                  &psEnc->sShape.LastGainIndex, condCoding == CODE_CONDITIONALLY, psEnc->sCmn.nb_subfr );

            /* Overwrite unquantized gains with quantized gains and convert back to Q0 from Q16 */
            for( i = 0; i < psEnc->sCmn.nb_subfr; i++ ) {
                sEncCtrl.Gains[ i ] = pGains_Q16[ i ] / 65536.0f;
            }

            /* Restore part of the input state */
            silk_memcpy( psRangeEnc, &sRangeEnc_copy, sizeof( ec_enc ) );
            silk_memcpy( &psEnc->sCmn.sNSQ, &sNSQ_copy, sizeof( silk_nsq_state ) );
            psEnc->sCmn.indices.Seed = seed_copy;
            psEnc->sCmn.ec_prevLagIndex = ec_prevLagIndex_copy;
            psEnc->sCmn.ec_prevSignalType = ec_prevSignalType_copy;
        }
    }

    /* Update input buffer */
    silk_memmove( psEnc->x_buf, &psEnc->x_buf[ psEnc->sCmn.frame_length ],
        ( psEnc->sCmn.ltp_mem_length + LA_SHAPE_MS * psEnc->sCmn.fs_kHz ) * sizeof( silk_float ) );

    /* Parameters needed for next frame */
    psEnc->sCmn.prevLag        = sEncCtrl.pitchL[ psEnc->sCmn.nb_subfr - 1 ];
    psEnc->sCmn.prevSignalType = psEnc->sCmn.indices.signalType;

    /* Exit without entropy coding */
    if( psEnc->sCmn.prefillFlag ) {
        /* No payload */
        *pnBytesOut = 0;
        return ret;
    }

    /****************************************/
    /* Finalize payload                     */
    /****************************************/
    psEnc->sCmn.first_frame_after_reset = 0;
    /* Payload size */
    *pnBytesOut = silk_RSHIFT( ec_tell( psRangeEnc ) + 7, 3 );

TOC(ENCODE_FRAME)

#ifdef SAVE_ALL_INTERNAL_DATA
    DEBUG_STORE_DATA( pitchL.dat,               sEncCtrl.pitchL,                                 MAX_NB_SUBFR * sizeof( opus_int   ) );
    DEBUG_STORE_DATA( pitchG_quantized.dat,     sEncCtrl.LTPCoef,            psEnc->sCmn.nb_subfr * LTP_ORDER * sizeof( silk_float ) );
    DEBUG_STORE_DATA( LTPcorr.dat,              &psEnc->LTPCorr,                                                sizeof( silk_float ) );
    DEBUG_STORE_DATA( gains.dat,                sEncCtrl.Gains,                          psEnc->sCmn.nb_subfr * sizeof( silk_float ) );
    DEBUG_STORE_DATA( gains_indices.dat,        &psEnc->sCmn.indices.GainsIndices,       psEnc->sCmn.nb_subfr * sizeof( opus_int8  ) );
    DEBUG_STORE_DATA( quantOffsetType.dat,      &psEnc->sCmn.indices.quantOffsetType,                           sizeof( opus_int8  ) );
    DEBUG_STORE_DATA( speech_activity_q8.dat,   &psEnc->sCmn.speech_activity_Q8,                                sizeof( opus_int   ) );
    DEBUG_STORE_DATA( signalType.dat,           &psEnc->sCmn.indices.signalType,                                sizeof( opus_int8  ) );
    DEBUG_STORE_DATA( lag_index.dat,            &psEnc->sCmn.indices.lagIndex,                                  sizeof( opus_int16 ) );
    DEBUG_STORE_DATA( contour_index.dat,        &psEnc->sCmn.indices.contourIndex,                              sizeof( opus_int8  ) );
    DEBUG_STORE_DATA( per_index.dat,            &psEnc->sCmn.indices.PERIndex,                                  sizeof( opus_int8  ) );
    DEBUG_STORE_DATA( PredCoef.dat,             &sEncCtrl.PredCoef[ 1 ],          psEnc->sCmn.predictLPCOrder * sizeof( silk_float ) );
    DEBUG_STORE_DATA( ltp_scale_idx.dat,        &psEnc->sCmn.indices.LTP_scaleIndex,                            sizeof( opus_int8   ) );
#endif
    return ret;
}

/* Low-Bitrate Redundancy (LBRR) encoding. Reuse all parameters but encode excitation at lower bitrate  */
static inline void silk_LBRR_encode_FLP(
    silk_encoder_state_FLP          *psEnc,             /* I/O  Encoder state FLP                       */
    silk_encoder_control_FLP        *psEncCtrl,         /* I/O  Encoder control FLP                     */
    const silk_float                 xfw[],              /* I    Input signal                            */
    opus_int                         condCoding         /* I    The type of conditional coding used so far for this frame */
)
{
    opus_int     k;
    opus_int32   Gains_Q16[ MAX_NB_SUBFR ];
    silk_float   TempGains[ MAX_NB_SUBFR ];
    SideInfoIndices *psIndices_LBRR = &psEnc->sCmn.indices_LBRR[ psEnc->sCmn.nFramesEncoded ];
    silk_nsq_state sNSQ_LBRR;

    /*******************************************/
    /* Control use of inband LBRR              */
    /*******************************************/
    if( psEnc->sCmn.LBRR_enabled && psEnc->sCmn.speech_activity_Q8 > SILK_FIX_CONST( LBRR_SPEECH_ACTIVITY_THRES, 8 ) ) {
        psEnc->sCmn.LBRR_flags[ psEnc->sCmn.nFramesEncoded ] = 1;

        /* Copy noise shaping quantizer state and quantization indices from regular encoding */
        silk_memcpy( &sNSQ_LBRR, &psEnc->sCmn.sNSQ, sizeof( silk_nsq_state ) );
        silk_memcpy( psIndices_LBRR, &psEnc->sCmn.indices, sizeof( SideInfoIndices ) );

        /* Save original gains */
        silk_memcpy( TempGains, psEncCtrl->Gains, psEnc->sCmn.nb_subfr * sizeof( silk_float ) );

        if( psEnc->sCmn.nFramesEncoded == 0 || psEnc->sCmn.LBRR_flags[ psEnc->sCmn.nFramesEncoded - 1 ] == 0 ) {
            /* First frame in packet or previous frame not LBRR coded */
            psEnc->sCmn.LBRRprevLastGainIndex = psEnc->sShape.LastGainIndex;

            /* Increase Gains to get target LBRR rate */
            psIndices_LBRR->GainsIndices[ 0 ] += psEnc->sCmn.LBRR_GainIncreases;
            psIndices_LBRR->GainsIndices[ 0 ] = silk_min_int( psIndices_LBRR->GainsIndices[ 0 ], N_LEVELS_QGAIN - 1 );
        }

        /* Decode to get gains in sync with decoder */
        silk_gains_dequant( Gains_Q16, psIndices_LBRR->GainsIndices,
            &psEnc->sCmn.LBRRprevLastGainIndex, condCoding == CODE_CONDITIONALLY, psEnc->sCmn.nb_subfr );

        /* Overwrite unquantized gains with quantized gains and convert back to Q0 from Q16 */
        for( k = 0; k <  psEnc->sCmn.nb_subfr; k++ ) {
            psEncCtrl->Gains[ k ] = Gains_Q16[ k ] * ( 1.0f / 65536.0f );
        }

        /*****************************************/
        /* Noise shaping quantization            */
        /*****************************************/
        silk_NSQ_wrapper_FLP( psEnc, psEncCtrl, psIndices_LBRR, &sNSQ_LBRR,
            psEnc->sCmn.pulses_LBRR[ psEnc->sCmn.nFramesEncoded ], xfw );

        /* Restore original gains */
        silk_memcpy( psEncCtrl->Gains, TempGains, psEnc->sCmn.nb_subfr * sizeof( silk_float ) );
    }
}