idf_component_register(
    SRCS
        "main.c"
        "board/board.c"
        "board/ch422g.c"
        "core/actions.c"
        "core/net.c"
        "core/signals.c"
        "core/storage.c"
        "core/template.c"
        "core/util.c"
        "io/io_can.c"
        "io/io_codec.c"
        "io/io_manager.c"
        "io/io_mqtt.c"
        "io/io_serial.c"
        "ui/ui_engine.c"
        "web/web_server.c"
    INCLUDE_DIRS "." "board" "core" "io" "ui" "web"
    EMBED_TXTFILES "default_layout.json"
)

# The web editor (www/) is gzipped at build time and embedded in the firmware.
idf_build_get_property(python PYTHON)
foreach(file index.html app.js style.css)
    set(src "${COMPONENT_DIR}/www/${file}")
    set(gz "${CMAKE_CURRENT_BINARY_DIR}/${file}.gz")
    string(MAKE_C_IDENTIFIER "www_${file}" target_name)
    add_custom_command(OUTPUT "${gz}"
        COMMAND "${python}" "${COMPONENT_DIR}/../tools/gzip_file.py" "${src}" "${gz}"
        DEPENDS "${src}" "${COMPONENT_DIR}/../tools/gzip_file.py"
        VERBATIM)
    add_custom_target(${target_name} DEPENDS "${gz}")
    target_add_binary_data(${COMPONENT_LIB} "${gz}" BINARY DEPENDS ${target_name})
endforeach()
