idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS ".")

# Allow build-time speed cap override: idf.py build -DSPEED_MAX_KMH=7.5
if(DEFINED SPEED_MAX_KMH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SPEED_MAX_KMH=${SPEED_MAX_KMH}f)
endif()
