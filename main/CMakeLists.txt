idf_component_register(
    SRCS "main.c"
         "wifi.c"
         "net_tls.c"
         "proto_frame.c"
         "proto_gen/polo.pb.c"
         "proto_gen/remotemessage.pb.c"
    INCLUDE_DIRS "." "proto_gen"
    PRIV_REQUIRES nvs_flash esp_wifi nanopb mbedtls
    EMBED_TXTFILES "../certs/client.pem" "../certs/client.key"
)
# EMBED_TXTFILES must be paired with board_build.embed_txtfiles in
# platformio.ini: the PlatformIO builder generates the .S files this
# CMake rule compiles.
