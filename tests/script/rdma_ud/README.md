# rdma_ud loop test gudei

## 1. Create the namespace

Customize the `TX_IF`, `RX_IF`, `TX_IP`, `RX_IP` as the setup.

```bash
./tests/script/rdma_ud/loop_ns_setup.sh
```

## 2. Loop test

### 2.1 Start a RX

```bash
sudo ip netns exec rdma1 ./build/app/RxTxApp --config_file tests/script/rdma_ud/rx_1v.json --p_port rdma_ud:enp175s0f1np1
```

### 2.2 Start a TX

```bash
sudo ip netns exec rdma0 ./build/app/RxTxApp --config_file tests/script/rdma_ud/tx_1v.json --p_port rdma_ud:enp175s0f0np0
```

