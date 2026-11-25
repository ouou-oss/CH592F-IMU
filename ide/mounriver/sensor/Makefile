# 与命令行共用构建参数，避免 IDE 生成另一套链接配置。
.PHONY: all debug size
all:
	$(MAKE) -C "../../../firmware" ROLE=sensor all
debug:
	$(MAKE) -C "../../../firmware" ROLE=sensor LOG=1 all
size:
	$(MAKE) -C "../../../firmware" ROLE=sensor size
