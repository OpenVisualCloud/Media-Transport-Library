import logging

logger = logging.getLogger(__name__)

rx: list[str] = []
rx_multicast: list[str] = []
tx: list[str] = []


def init(session_id: int, pool_size: int = 8) -> None:
    if pool_size > 128:
        logger.warning(f"Pool size was too big ({pool_size} > 128). Set to 128.")
        pool_size = 128
    for i in range(pool_size):
        rx.append(f"192.168.{session_id}.{i}")
        rx_multicast.append(f"239.0.{session_id}.{i}")
        tx.append(f"192.168.{session_id}.{pool_size + i}")
