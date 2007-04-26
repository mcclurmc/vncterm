include $(B_BASE)/common.mk

REPO := $(call hg_loc,vncterm)

.PHONY: build
build:
	make -C $(REPO)
	cp -v $(REPO)/vncterm $(MY_OUTPUT_DIR)/vncterm

.PHONY: clean
clean:
	make -C $(REPO) clean

