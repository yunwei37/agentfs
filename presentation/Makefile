.PHONY: dev build export clean install

# Install dependencies
install:
	pnpm install

# Start dev server
dev:
	pnpm run dev

# Build for deployment
build:
	pnpm run build

# Export to PDF
export:
	SLIDEV_EXPORT=1 pnpm run export

# Clean build artifacts
clean:
	rm -rf dist branchcontext-slides.pdf
