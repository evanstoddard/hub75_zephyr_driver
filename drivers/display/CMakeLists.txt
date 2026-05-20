zephyr_library()

zephyr_library_sources_ifdef(CONFIG_HUB75 display_hub75.c)
