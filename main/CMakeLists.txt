idf_component_register(
    SRCS "main.c"
         "focus_model.c"
         "heatmap_store.c"
         "app_clock.c"
         "app_store.c"
         "app_audio.c"
         "app_ui.c"
         "app_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_timer nvs_flash esp_wifi esp_netif esp_event esp_http_server
)

# 番茄钟应用的中文子集字库(来源/许可/再生成见 assets/fonts/)。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_16.c"
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/app_font_20.c"
)
