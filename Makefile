# Thin wrappers around the documented developer commands. CMake remains the
# build implementation; this file must not duplicate its dependency graph.

UV     := uv run --frozen --group tooling
PRESET ?= dev

.DEFAULT_GOAL := help
.PHONY: help hooks lint configure build test gui-wayland gui-x11
.PHONY: visual visual-record tidy coverage coverage-open clean

help: ## Show the available targets
	@grep -hE '^[a-zA-Z0-9_-]+:.*?## ' $(MAKEFILE_LIST) \
		| awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-16s\033[0m %s\n", $$1, $$2}'

hooks: ## Install the pinned tooling environment and Git hooks
	uv sync --locked --no-build --group tooling
	$(UV) pre-commit install --install-hooks

lint: ## Run the complete pre-commit suite against all tracked files
	$(UV) pre-commit run --all-files --show-diff-on-failure

configure: ## Configure the build tree (PRESET=dev by default)
	cmake --preset $(PRESET)

build: ## Build the configured tree
	cmake --build --preset $(PRESET)

test: ## Run the test suite for the configured preset
	ctest --preset $(PRESET) --output-on-failure

gui-wayland: ## Run the GUI suite under an isolated headless Wayland session
	tests/support/with-wayland.sh ctest --preset $(PRESET) --label-regex gui --output-on-failure

gui-x11: ## Run the GUI suite under an isolated X11 session (compatibility)
	tests/support/with-x11.sh ctest --preset $(PRESET) --label-regex gui --output-on-failure

visual: ## Compare the rendered views against the checked-in reference images
	ctest --preset $(PRESET) --label-regex visual --output-on-failure

visual-record: ## Re-record the reference images, then review the diff before committing
	CULLFINCH_UPDATE_VISUAL_REFERENCES=1 ctest --preset $(PRESET) --label-regex visual \
		--output-on-failure
	@git diff --stat tests/fixtures/visual

tidy: ## Run a fresh clang-tidy analysis build
	cmake --preset ci-tidy-linux
	cmake --build --preset ci-tidy-linux

coverage: ## Build, test and generate the coverage report
	cmake --preset coverage
	cmake --build --preset coverage --target coverage-reset
	cmake --build --preset coverage
	ctest --preset coverage --output-on-failure
	cmake --build --preset coverage --target coverage-report

coverage-open: coverage ## Generate the coverage report and open the HTML view
	@echo "Report: build/coverage/coverage/html/index.html"

clean: ## Remove build trees and coverage output (not the vcpkg cache)
	rm -rf build coverage
