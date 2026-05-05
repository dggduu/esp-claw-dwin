file(GLOB_RECURSE SILK_SRCS "silk/src/*.c")

set(_excluded_patterns
    "SKP_Silk_enc_API"
    "SKP_Silk_encode_frame"
    "SKP_Silk_encode_parameters"
    "SKP_Silk_encode_pulses"
    "SKP_Silk_init_encoder"
    "SKP_Silk_NSQ"
    "SKP_Silk_noise_shape_analysis"
    "SKP_Silk_process_gains"
    "SKP_Silk_process_NLSFs"
    "SKP_Silk_NLSF_MSVQ_encode"
    "SKP_Silk_NLSF_VQ_rate_distortion"
    "SKP_Silk_VAD"
    "SKP_Silk_HP_variable_cutoff"
    "SKP_Silk_find_LTP"
    "SKP_Silk_find_LPC"
    "SKP_Silk_find_pitch_lags"
    "SKP_Silk_find_pred_coefs"
    "SKP_Silk_prefilter"
    "SKP_Silk_quant_LTP"
    "SKP_Silk_regularize_corr"
    "SKP_Silk_residual_energy"
    "SKP_Silk_control_audio_bandwidth"
    "SKP_Silk_detect_SWB"
)

foreach(pattern ${_excluded_patterns})
    list(FILTER SILK_SRCS EXCLUDE REGEX ".*${pattern}.*")
endforeach()

idf_component_register(
    SRCS ${SILK_SRCS}
    INCLUDE_DIRS
        "silk/interface"
        "silk/src"
)
