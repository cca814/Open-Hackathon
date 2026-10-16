P4C ?= p4c-bm2-ss

.PHONY: build
build:
	mkdir -p build
	$(P4C) --std p4-16 --p4runtime-files build/main.p4info.txt -o build/main.json p4/main.p4
