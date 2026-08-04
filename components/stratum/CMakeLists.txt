idf_component_register(
SRCS
    "utils.c"
    "mining.c"
    "stratum_api.c"
    "stratum_socket.c"
    "coinbase_decoder.c"
    "segwit_addr.c"
    "base58.c"

INCLUDE_DIRS
    "include"

REQUIRES
    "json"
    "mbedtls"
    "app_update"
    # Gate W6.1: the restart request site is recorded at the boundary that
    # owns it. Without CONFIG_NX_MUTATION_OBSERVABILITY the call compiles
    # away and nothing is linked.
    "mutation_observability"
    "esp_timer"
    "tcp_transport"
    "esp_netif"
)
