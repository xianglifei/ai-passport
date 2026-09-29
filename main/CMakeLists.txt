idf_component_register(
    SRCS "main.c"
         "quota_model.c"
         "quota_fetch.c"
         "focus_model.c"
         "focus_store.c"
         "focus_sync.c"
         "app_audio.c"
         "app_clock.c"
         "app_store.c"
         "app_ui.c"
         "app_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer nvs_flash esp_wifi esp_netif esp_event
             esp_http_server esp_http_client json mbedtls
)

# 应用的中文子集字库(来源/许可/再生成见 assets/fonts/)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_20.c"
)
