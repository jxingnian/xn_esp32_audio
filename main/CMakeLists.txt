idf_component_register(SRCS "main.c"
                           "audio_config_app.c"
                       PRIV_REQUIRES xn_web_wifi_manger xn_audio_manager
                       INCLUDE_DIRS "")
