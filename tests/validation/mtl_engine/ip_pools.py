import logging

logger = logging.getLogger(__name__)

rx: list[str] = []
rx_multicast: list[str] = []
tx: list[str] = []

# Redundant (secondary port) pools — uses session_id + 1 as subnet
tx_r: list[str] = []
rx_r: list[str] = []


def init(session_id: int, pool_size: int = 8) -> None:
    if pool_size > 128:
        logger.warning(f"Pool size was too big ({pool_size} > 128). Set to 128.")
        pool_size = 128
    for i in range(pool_size):
        host_octet = i + 1
        rx.append(f"192.168.{session_id}.{host_octet}")
        rx_multicast.append(f"239.0.{session_id}.{host_octet}")
        tx.append(f"192.168.{session_id}.{pool_size + i}")

        # Secondary port for ST2022-7 redundant sessions
        r_subnet = session_id + 1
        tx_r.append(f"192.168.{r_subnet}.{pool_size + i}")
        rx_r.append(f"192.168.{r_subnet}.{host_octet}")
