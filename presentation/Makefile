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
	SLIDEV_EXPORT=1 npx slidev export --output agentfs-slides.pdf

# Clean build artifacts
clean:
	rm -rf dist agentfs-slides.pdf
