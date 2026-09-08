set(C2_SOURCE_NAMES
    codec2.c codec2_fft.c dump.c lpc.c nlp.c postfilter.c sine.c
    kiss_fft.c kiss_fftr.c interp.c lsp.c mbest.c newamp1.c
    phase.c quantise.c pack.c)
set(C2_SOURCES)
foreach(name IN LISTS C2_SOURCE_NAMES)
    list(APPEND C2_SOURCES "${CMAKE_CURRENT_LIST_DIR}/vendor/src/${name}")
endforeach()
foreach(name codebook codebookd codebookjmv codebookge codebooknewamp1 codebooknewamp1_energy)
    list(APPEND C2_SOURCES "${CMAKE_CURRENT_LIST_DIR}/generated/${name}.c")
endforeach()
