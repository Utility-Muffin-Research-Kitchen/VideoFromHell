# Video From Hell — Leaf video player.
#
# This repository owns its package build. Leaf may explicitly stage it for
# developer acceptance, but the app is Pak Rat-owned and never belongs in the
# default Leaf payload.

MLP1_PACKAGE := build/mlp1/package/VideoFromHell.pak
MLP1_ARCHIVE := build/mlp1/VideoFromHell.pak.zip
MLP1_BIN     := ports/mlp1/pak/bin/videofromhell
PAK_VERSION  := $(shell python3 -c 'import json; print(json.load(open("pak/pak.json"))["pak_version"])')

CJSON_DIR ?= ../Jawaka/third_party/cjson
STUB_SOURCES := third_party/ffmpeg/stub/vfh_avformat_stub.c \
	third_party/ffmpeg/stub/vfh_avcodec_stub.c \
	third_party/ffmpeg/stub/vfh_avutil_stub.c \
	third_party/ffmpeg/stub/vfh_swresample_stub.c \
	third_party/ffmpeg/stub/vfh_swscale_stub.c \
	third_party/mpp/stub/vfh_mpp_stub.c

.PHONY: package-platform package-mlp1 package-archive package-smoke dist-pakrat mlp1 probe-mlp1 player-smoke-mlp1 media-smoke-mlp1 media-fixture-smoke-mlp1 test pakrat-metadata-check stubs-test sources-test library-test art-test osd-test resume-test queue-test status-test srt-test launch-test clean

test: pakrat-metadata-check stubs-test sources-test library-test art-test osd-test resume-test queue-test status-test srt-test launch-test

pakrat-metadata-check:
	@python3 scripts/pakrat-metadata-check.py

stubs-test:
	@mkdir -p build/tests/stubs
	@set -e; for source in $(STUB_SOURCES); do \
		object="build/tests/stubs/$$(basename "$${source%.c}").o"; \
		$(CC) -std=c11 -Wall -Wextra -Werror \
			-Ithird_party/ffmpeg/include -Ithird_party/mpp/include \
			-c "$$source" -o "$$object"; \
	done

sources-test:
	@mkdir -p build/tests
	@$(CC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Icmd/videofromhell \
		cmd/videofromhell/vfh_sources.c cmd/videofromhell/vfh_sources_test.c \
		-o build/tests/vfh-sources-test
	@build/tests/vfh-sources-test

library-test:
	@mkdir -p build/tests
	@$(CC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Icmd/videofromhell -I$(CJSON_DIR) \
		cmd/videofromhell/vfh_sources.c cmd/videofromhell/vfh_library.c \
		cmd/videofromhell/vfh_library_test.c $(CJSON_DIR)/cJSON.c \
		-o build/tests/vfh-library-test
	@build/tests/vfh-library-test

art-test:
	@mkdir -p build/tests
	@$(CC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Icmd/videofromhell \
		cmd/videofromhell/vfh_art.c cmd/videofromhell/vfh_art_test.c \
		-o build/tests/vfh-art-test
	@build/tests/vfh-art-test

osd-test:
	@mkdir -p build/tests
	@$(CC) -std=c11 -Wall -Wextra -Werror -Icmd/videofromhell \
		cmd/videofromhell/vfh_osd.c cmd/videofromhell/vfh_osd_test.c \
		-o build/tests/vfh-osd-test
	@build/tests/vfh-osd-test

resume-test:
	@mkdir -p build/tests
	@$(CC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Icmd/videofromhell -I$(CJSON_DIR) \
		cmd/videofromhell/vfh_resume.c cmd/videofromhell/vfh_resume_test.c \
		$(CJSON_DIR)/cJSON.c -o build/tests/vfh-resume-test
	@build/tests/vfh-resume-test

queue-test:
	@mkdir -p build/tests
	@$(CC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Icmd/videofromhell -I$(CJSON_DIR) \
		cmd/videofromhell/vfh_queue.c cmd/videofromhell/vfh_resume.c \
		cmd/videofromhell/vfh_queue_test.c \
		$(CJSON_DIR)/cJSON.c -o build/tests/vfh-queue-test
	@build/tests/vfh-queue-test

status-test:
	@mkdir -p build/tests
	@$(CC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -Icmd/videofromhell -I$(CJSON_DIR) \
		cmd/videofromhell/vfh_status.c cmd/videofromhell/vfh_status_test.c \
		$(CJSON_DIR)/cJSON.c -lpthread -o build/tests/vfh-status-test
	@build/tests/vfh-status-test

srt-test:
	@mkdir -p build/tests
	@$(CC) -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Icmd/videofromhell \
		cmd/videofromhell/vfh_srt.c cmd/videofromhell/vfh_srt_test.c \
		-o build/tests/vfh-srt-test
	@build/tests/vfh-srt-test

launch-test:
	@sh ./scripts/launch-smoke.sh

package-platform:
	@test -n "$(PLATFORM)" || { echo "usage: make package-platform PLATFORM=<platform>" >&2; exit 1; }
	@case "$(PLATFORM)" in \
		mlp1) $(MAKE) package-mlp1 ;; \
		*) echo "unsupported Video From Hell package platform: $(PLATFORM)" >&2; exit 1 ;; \
	esac

mlp1:
	@./scripts/build-mlp1.sh

# Phase 5 gate: test-only MPP throughput binary, intentionally outside the Pak.
probe-mlp1:
	@./scripts/build-mlp1.sh vfh-probe

# Test-only acceptance runner: opens the real player and requires NV12 output.
player-smoke-mlp1:
	@./scripts/build-mlp1.sh vfh-player-smoke

# Test-only acceptance runner for VFH's metadata + poster decoder path.
media-smoke-mlp1:
	@./scripts/build-mlp1.sh vfh-media-smoke

# Generates disposable device-side fixtures for the poster acceptance gates.
media-fixture-smoke-mlp1: media-smoke-mlp1
	@./scripts/media-fixture-smoke-mlp1.sh

package-mlp1: mlp1
	@rm -rf "$(MLP1_PACKAGE)"
	@mkdir -p "$(MLP1_PACKAGE)/bin"
	@cp -R pak/launch.sh pak/pak.json pak/res "$(MLP1_PACKAGE)/"
	@cp "$(MLP1_BIN)" "$(MLP1_PACKAGE)/bin/videofromhell"
	@echo "=== Packaged: $(MLP1_PACKAGE) ==="

package-archive: package-mlp1
	@python3 scripts/package-mlp1.py \
		--package "$(MLP1_PACKAGE)" \
		--archive "$(MLP1_ARCHIVE)"

package-smoke: package-archive
	@python3 scripts/package-smoke.py \
		--package "$(MLP1_PACKAGE)" \
		--archive "$(MLP1_ARCHIVE)" \
		--version "$(PAK_VERSION)"

# Alias used by release automation once the app is ready to publish.
dist-pakrat: package-archive

clean:
	rm -rf build ports/*/build ports/*/pak/bin
