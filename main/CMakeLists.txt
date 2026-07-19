idf_component_register(
    SRCS "main.c"
         "wifi.c"
         "proto_gen/polo.pb.c"
         "proto_gen/remotemessage.pb.c"
    INCLUDE_DIRS "." "proto_gen"
    PRIV_REQUIRES nvs_flash esp_wifi nanopb
)
