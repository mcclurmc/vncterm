USE_BRANDING := yes
IMPORT_BRANDING := yes
include $(B_BASE)/common.mk
include $(B_BASE)/rpmbuild.mk

REPO_NAME= vncterm
RPM_BUILD_COOKIE= $(MY_OBJ_DIR)/.rpm_build_cookie

J_FLAG = 8
MAKE_CMD= $(MAKE) -j$(J_FLAG) -C $(call hg_loc,vncterm)

-include $(MY_OBJ_DIR)/version.inc
$(MY_OBJ_DIR)/version.inc:
	rm -f $(MY_OBJ_DIR)/version.inc
	$(version-makefile) > $@
	$(call hg_cset_number,vncterm) >> $@
	echo VNCTERM_VERSION := \$$\(PLATFORM_VERSION\) >> $@
	echo VNCTERM_RELEASE := \$$\(CSET_NUMBER\) >> $@

CHANGELOG_DATE    := $(shell LANG=C date +"%a %b %d %Y")
CHANGELOG_USER    := $(COMPANY_NAME) <$(COMPANY_WEBSITE)>
CHANGELOG_HEADER  := * $(CHANGELOG_DATE) $(CHANGELOG_USER) [$(VNCTERM_VERSION) $(VNCTERM_RELEASE)]
CHANGELOG_MESSAGE := - Build vncterm.

SRPM := $(RPM_SRPMSDIR)/vncterm-$(VNCTERM_VERSION)-$(VNCTERM_RELEASE).src.rpm

.PHONY: build
build: $(RPM_BUILD_COOKIE) \
	$(MY_SOURCES)/MANIFEST
	@ :

.PHONY: clean
clean:
	rm -f $(RPM_BUILD_COOKIE)
	rm -f $(MY_OBJ_DIR)/version.inc
	rm -f $(MY_SOURCES)/MANIFEST

SOURCES = $(RPM_SOURCESDIR)/vncterm-$(VNCTERM_VERSION).tar.bz2 \
	  $(RPM_SOURCESDIR)/vncterm-development.patch \
	  $(RPM_SPECSDIR)/vncterm.spec

$(RPM_SOURCESDIR)/vncterm-$(VNCTERM_VERSION).tar.bz2: $(RPM_SOURCESDIR)/.dirstamp $(call hg_loc,vncterm)
	hg -R $(call hg_loc,vncterm) archive -t tbz2 $@

$(RPM_SOURCESDIR)/vncterm-development.patch: $(RPM_SOURCESDIR)/.dirstamp $(call hg_loc,vncterm)
	hg -R $(call hg_loc,vncterm) diff > $@

$(RPM_SPECSDIR)/vncterm.spec: vncterm.spec.in $(RPM_SPECSDIR)/.dirstamp
# Use a temporary file because sed in rhel3 doesn't understand "\n"
	echo "$(CHANGELOG_HEADER)" > changelog.tmp
	echo "$(CHANGELOG_MESSAGE)" >> changelog.tmp
	echo "" >> changelog.tmp
	sed -e s\,@REPO_VERSION@,$(REPO_VERSION),g \
	    -e s\,@VNCTERM_VERSION@,$(VNCTERM_VERSION),g\
	    -e s\,@VNCTERM_RELEASE@,$(VNCTERM_RELEASE),g\
	    -e \\,%changelog,rchangelog.tmp             \
	    < vncterm.spec.in                           \
	    > $(RPM_SPECSDIR)/vncterm.spec
	-rm changelog.tmp

$(RPM_BUILD_COOKIE): $(RPM_DIRECTORIES) $(SOURCES)
	$(RPMBUILD) --target i686 -ba $(RPM_SPECSDIR)/vncterm.spec
	@touch $@

$(MY_SOURCES)/MANIFEST: $(MY_SOURCES)/.dirstamp
	echo "$(REPO_NAME) $(call rpm_license,$(SRPM)) file $(SRPM)" > $@
