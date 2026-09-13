################################################################################
# Docker build/run helpers
#
# Usage:
#   make build       Download extern/ sources + Build the Docker image
#   make build-nc    Download + Build without cache
#   make run         Run the container (interactive)
#   make test        Build + Test in container
#   make extern      Download/extract all extern/ sources
################################################################################

# Docker variables
DOCKER_IMAGE    := sysrepo-mcp-server
DOCKER_TAG      := latest
DOCKERFILE	:= Dockerfile
DOCKER_CONTEXT  := ..

# Extern sources base directory
EXTERNDIR  := ../extern

# Extern sources URLs
LIBYANG_URL  := https://github.com/CESNET/libyang/archive/refs/tags/v5.8.6.tar.gz
SYSREPO_URL  := https://github.com/sysrepo/sysrepo/archive/refs/tags/v5.1.0.tar.gz
EBUILD_URL   := https://github.com/grgbr/ebuild/archive/refs/heads/master.zip
STROLL_URL   := https://github.com/grgbr/stroll/archive/refs/heads/master.zip
UTILS_URL    := https://github.com/grgbr/utils/archive/refs/heads/master.zip
ELOG_URL     := https://github.com/grgbr/elog/archive/refs/heads/master.zip
FCGI2_URL    := https://github.com/FastCGI-Archives/fcgi2/archive/refs/tags/2.4.7.tar.gz

# Extern source directories (no version in path)
LIBYANG_DIR  := $(EXTERNDIR)/libyang
SYSREPO_DIR  := $(EXTERNDIR)/sysrepo
EBUILD_DIR   := $(EXTERNDIR)/ebuild
STROLL_DIR   := $(EXTERNDIR)/stroll
UTILS_DIR    := $(EXTERNDIR)/utils
ELOG_DIR     := $(EXTERNDIR)/elog
FCGI2_DIR    := $(EXTERNDIR)/fcgi2

.PHONY: build build-nc run test extern extern-clean

# ---------------------------------------------------------------------------
# Extern sources management
# ---------------------------------------------------------------------------

# Aggregate target: download and extract all extern sources
extern: $(LIBYANG_DIR) $(SYSREPO_DIR) $(EBUILD_DIR) $(STROLL_DIR) \
        $(UTILS_DIR) $(ELOG_DIR) $(FCGI2_DIR)
	@echo "==> All extern sources ready."

# Create extern/ directory
$(EXTERNDIR):
	mkdir -p $(@)

# libyang
$(LIBYANG_DIR): | $(EXTERNDIR)
	@echo "==> Downloading libyang ..."
	curl -sL $(LIBYANG_URL) | tar xz -C $(EXTERNDIR)
	mv -f $(EXTERNDIR)/libyang-5.8.6 $(@)
	touch $@

# sysrepo
$(SYSREPO_DIR): | $(EXTERNDIR)
	@echo "==> Downloading sysrepo ..."
	curl -sL $(SYSREPO_URL) | tar xz -C $(EXTERNDIR)
	mv -f $(EXTERNDIR)/sysrepo-5.1.0 $(@)
	touch $@

# ebuild (zip)
$(EBUILD_DIR): | $(EXTERNDIR)
	@echo "==> Downloading ebuild (master) ..."
	curl -sL $(EBUILD_URL) -o /tmp/ebuild.zip
	unzip -o /tmp/ebuild.zip -d $(EXTERNDIR)
	mv $(EXTERNDIR)/ebuild-master $(EBUILD_DIR)
	rm -f /tmp/ebuild.zip
	@touch $@

# stroll (zip)
$(STROLL_DIR): | $(EXTERNDIR)
	@echo "==> Downloading stroll (master) ..."
	curl -sL $(STROLL_URL) -o /tmp/stroll.zip
	unzip -o /tmp/stroll.zip -d $(EXTERNDIR)
	mv $(EXTERNDIR)/stroll-master $(STROLL_DIR)
	rm -f /tmp/stroll.zip
	@touch $@

# utils (zip)
$(UTILS_DIR): | $(EXTERNDIR)
	@echo "==> Downloading utils (master) ..."
	curl -sL $(UTILS_URL) -o /tmp/utils.zip
	unzip -o /tmp/utils.zip -d $(EXTERNDIR)
	mv $(EXTERNDIR)/utils-master $(UTILS_DIR)
	rm -f /tmp/utils.zip
	@touch $@

# elog (zip)
$(ELOG_DIR): | $(EXTERNDIR)
	@echo "==> Downloading elog (master) ..."
	curl -sL $(ELOG_URL) -o /tmp/elog.zip
	unzip -o /tmp/elog.zip -d $(EXTERNDIR)
	mv $(EXTERNDIR)/elog-master $(ELOG_DIR)
	rm -f /tmp/elog.zip
	@touch $@

# fcgi2
$(FCGI2_DIR): | $(EXTERNDIR)
	@echo "==> Downloading fcgi2 ..."
	curl -sL $(FCGI2_URL) | tar xz -C $(EXTERNDIR)
	mv -f $(EXTERNDIR)/fcgi2-2.4.7 $(@)
	touch $@

# Clean extern sources
extern-clean:
	@echo "==> Cleaning extern/ sources ..."
	rm -rf $(EXTERNDIR)

# ---------------------------------------------------------------------------
# Docker image targets
# ---------------------------------------------------------------------------

# Download the extern sources then build the image (cached)
build: $(LIBYANG_DIR) $(SYSREPO_DIR) $(EBUILD_DIR) $(STROLL_DIR) $(UTILS_DIR) \
       $(ELOG_DIR) $(FCGI2_DIR)
	@echo "==> Building Docker image $(DOCKER_IMAGE):$(DOCKER_TAG) ..."
	docker build -t "$(DOCKER_IMAGE):$(DOCKER_TAG)" -f "$(DOCKERFILE)" "$(DOCKER_CONTEXT)"
	@echo "==> Docker image built: $(DOCKER_IMAGE):$(DOCKER_TAG)"

# Download the extern sources then build the image without cache
build-nc: $(LIBYANG_DIR) $(SYSREPO_DIR) $(EBUILD_DIR) $(STROLL_DIR) $(UTILS_DIR) \
          $(ELOG_DIR) $(FCGI2_DIR)
	@echo "==> Building Docker image without cache ..."
	docker build --no-cache -t "$(DOCKER_IMAGE):$(DOCKER_TAG)" -f "$(DOCKERFILE)" "$(DOCKER_CONTEXT)"
	@echo "==> Docker image built: $(DOCKER_IMAGE):$(DOCKER_TAG)"

run:
	@echo "==> Running Docker container interactively ..."
	docker run --rm -it -u "$(id -u):$(id -g)" -v "$$PWD":/home/builder/project "$(DOCKER_IMAGE):$(DOCKER_TAG)" bash

test: build
	@echo "==> Running build + test in container ..."
	docker run --rm -u "$(id -u):$(id -g)" -v "$$PWD":/home/builder/project "$(DOCKER_IMAGE):$(DOCKER_TAG)" \\
		/home/builder/project/scripts/test.sh
	@echo "==> Build + test complete!"
