#!/bin/bash
# Source this file to set InfluxDB env-vars for POC binaries.
# Usage:  source poc/monitoring/env.sh

export INFLUXDB_URL="${INFLUXDB_URL:-http://localhost:8086}"
export INFLUXDB_TOKEN="${INFLUXDB_TOKEN:-mtl-mxl-poc-token}"
export INFLUXDB_ORG="${INFLUXDB_ORG:-mtl-mxl}"
export INFLUXDB_BUCKET="${INFLUXDB_BUCKET:-poc}"

echo "[env] InfluxDB push configured → $INFLUXDB_URL  org=$INFLUXDB_ORG  bucket=$INFLUXDB_BUCKET"
