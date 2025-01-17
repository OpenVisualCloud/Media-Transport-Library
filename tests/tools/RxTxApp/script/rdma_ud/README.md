# rdma_ud loop test guide

## 1. Create the namespace

Customize the `TX_IF`, `RX_IF`, `TX_IP`, `RX_IP` as the setup.

```bash
./tests/tools/RxTxApp/script/rdma_ud/loop_ns_setup.sh
```

## 2. Loop test

### 2.1. Start a RX

```bash
./tests/tools/RxTxApp/build/RxTxApp --config_file tests/script/rdma_ud/rx_1v.json
```

### 2.2. Start a TX

```bash
./tests/tools/RxTxApp/build/RxTxApp --config_file tests/script/rdma_ud/tx_1v.json
```
