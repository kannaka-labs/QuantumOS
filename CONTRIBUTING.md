# Contributing to QuantumOS

Thank you for your interest in contributing to QuantumOS! This guide will help you get started with contributing to this next-generation quantum-aware operating system.

## Table of Contents
- [Code of Conduct](#code-of-conduct)
- [Getting Started](#getting-started)
- [Development Workflow](#development-workflow)
- [Coding Standards](#coding-standards)
- [Testing Requirements](#testing-requirements)
- [Pull Request Process](#pull-request-process)
- [Issue Guidelines](#issue-guidelines)
- [Security](#security)

## AI Contributor Guidelines

QuantumOS is an AI-collaborative project where multiple AI models contribute alongside human developers. This section establishes guidelines to prevent drift and ensure quality across AI contributors.

### Pre-Submission Requirements

Before submitting ANY code, AI contributors MUST complete these verifications:

#### 1. API Consistency Check
```bash
# REQUIRED: Run before every PR
./scripts/check-api-consistency.sh
```

- [ ] All function calls reference functions that ACTUALLY EXIST in headers
- [ ] Function signatures match declarations exactly
- [ ] Return types are handled correctly
- [ ] No "phantom" functions (calling APIs that don't exist)

#### 2. Compilation Verification
```bash
# REQUIRED: Must pass before PR
make clean && make
```

- [ ] Code compiles without errors
- [ ] No new warnings introduced
- [ ] All dependencies declared in headers

#### 3. Documentation Alignment
- [ ] Code examples in docs match actual API
- [ ] No references to non-existent functions
- [ ] When fixing code, also fix related docs

### Common Drift Patterns (AVOID THESE)

Based on observed issues in this project:

**Pattern 1: Phantom API Calls**
```c
// WRONG - These functions don't exist!
ipc_create_queue(pid, &queue_id);
ipc_destroy_queue(queue_id);

// CORRECT - Use the ACTUAL API from ipc.h
ipc_process_init(pid);
ipc_process_cleanup(pid);
```
*Prevention*: READ the header file before calling functions from it.

**Pattern 2: Incomplete Fixes**
Claiming to fix something but only partially doing so (e.g., fixing code but not docs).
*Prevention*: After making a fix, grep for the old pattern everywhere.

**Pattern 3: Assumed Context**
Assuming functions exist based on naming conventions without verifying.
*Prevention*: Always verify by reading actual source files.

### Commit Message Format for AI Contributors

```
type(scope): description

Detailed explanation of changes.

AI-Contributor: Model-Name/Version
AI-Confidence: high|medium|low
AI-Verified: api-check,compile,docs
Co-Authored-By: Model Name <noreply@provider.com>
```

**AI-Contributor**: Model name (Claude-Opus-4.5, GPT-4, Gemini-3, etc.)
**AI-Confidence**: Self-assessed confidence level
**AI-Verified**: Comma-separated verifications performed

### Quality Tracking

AI contributions are tracked with these metrics:

| Metric | Target | Description |
|--------|--------|-------------|
| API Accuracy | 100% | Function calls match existing APIs |
| First-Compile Success | 100% | Code compiles on first submission |
| Review Iterations | < 2 | Rounds before merge-ready |
| Doc Consistency | 100% | Docs match implementation |

### Verification Scripts

```bash
# Check API consistency (function calls vs declarations)
./scripts/check-api-consistency.sh

# Full pre-PR validation
./scripts/validate-contribution.sh

# Check for common drift patterns
./scripts/lint-check.sh
```

---

## Code of Conduct

### Our Pledge
We are committed to providing a welcoming and inclusive environment for all contributors, regardless of:
- Experience level with operating systems
- Familiarity with quantum computing
- Background or identity
- Geographic location

### Standards
- Use welcoming and inclusive language
- Be respectful of differing viewpoints and experiences
- Gracefully accept constructive criticism
- Focus on what is best for the community
- Show empathy toward other community members

### Enforcement
Instances of abusive, harassing, or otherwise unacceptable behavior may be reported to the project maintainers.

## Getting Started

### Prerequisites
- C programming experience
- Basic understanding of operating systems concepts
- Familiarity with quantum computing concepts (helpful but not required)
- Cross-compilation tools (see Bootstrap Guide)

### Development Environment Setup
```bash
# Clone the repository
git clone https://github.com/kannaka-labs/QuantumOS.git
cd QuantumOS

# Install dependencies
make install-deps

# Build and test
make && make run
```

### First Contribution
1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Add tests
5. Submit a pull request

## Development Workflow

### Branch Strategy
- **main** - the single integration branch; every PR targets it and CI must be green to merge
- **feature/*** , **docs/*** , **fix/*** - short-lived branches, squash-merged into `main`
- Releases are cut by pushing a `vX.Y.Z` tag (matching the `VERSION` file), which fires
  `.github/workflows/release.yml` — there is no long-lived `develop` or `release` branch

### Commit Guidelines
Use conventional commits:
- `feat:` - New features
- `fix:` - Bug fixes
- `docs:` - Documentation changes
- `style:` - Code style changes
- `refactor:` - Code refactoring
- `test:` - Test additions/changes
- `chore:` - Build process or auxiliary tool changes

Examples:
```
feat(process): add process creation API
fix(memory): resolve page table mapping bug
docs(readme): update installation instructions
```

### Code Review Process
1. All changes require review
2. At least one maintainer approval
3. Automated tests must pass
4. Documentation must be updated
5. Security changes require additional review

## Coding Standards

### General Guidelines
- Follow Linux kernel coding style
- Use 4-space indentation (no tabs)
- 80-character line limit where practical
- Comment non-obvious code
- Use descriptive variable names

### Automatic Formatting
The repository ships a [`.clang-format`](.clang-format) that codifies the
project style (4-space indent, attached braces, right-aligned pointers,
100-column limit, include order preserved). CI enforces it, so format
before committing:

```bash
# Format the files you touched
clang-format -i path/to/changed_file.c

# Or check without modifying (what CI runs)
clang-format --dry-run --Werror path/to/changed_file.c
```

CI installs clang-format from ubuntu-latest apt (currently major version
18); if your local version disagrees with CI, prefer the CI verdict.

### C Code Style
```c
// Good example
int process_create(const char *name, uint32_t parent_pid, process_t **process)
{
    if (!name || !process) {
        return -EINVAL;
    }
    
    *process = kmalloc(sizeof(process_t));
    if (!*process) {
        return -ENOMEM;
    }
    
    memset(*process, 0, sizeof(process_t));
    strncpy((*process)->name, name, sizeof((*process)->name) - 1);
    (*process)->parent_pid = parent_pid;
    
    return 0;
}
```

### Header Organization
```c
#ifndef FILENAME_H
#define FILENAME_H

#include <kernel/types.h>
#include <kernel/memory.h>

/* Constants */
#define MAX_PROCESSES 1024

/* Type definitions */
typedef struct process process_t;

/* Function declarations */
int process_create(const char *name, uint32_t parent_pid, process_t **process);

#endif /* FILENAME_H */
```

### Documentation
Use Doxygen-style comments:
```c
/**
 * @brief Create a new process
 * 
 * @param name Process name (must be null-terminated)
 * @param parent_pid Parent process ID
 * @param process Output pointer to created process
 * @return int 0 on success, negative error code on failure
 * 
 * @note The caller is responsible for freeing the process with process_destroy()
 */
int process_create(const char *name, uint32_t parent_pid, process_t **process);
```

## Testing Requirements

### Test Categories
- **Unit Tests** - Test individual functions in isolation
- **Integration Tests** - Test component interaction
- **System Tests** - Test complete workflows
- **Performance Tests** - Verify performance targets
- **Security Tests** - Verify security properties

### Test Structure
```
tests/
├── unit/
│   ├── test_memory.c
│   ├── test_process.c
│   └── test_ipc.c
├── integration/
│   ├── test_kernel_services.c
│   └── test_quantum_integration.c
├── system/
│   ├── test_boot_sequence.c
│   └── test_quantum_workloads.c
└── benchmarks/
    ├── performance.c
    └── scalability.c
```

### Running Tests

QuantumOS is a freestanding kernel (`-nostdlib`), so it is not unit-tested like a library —
`gcov` cannot instrument it. Assurance comes from **serial-log integration gates**: the kernel
boots in headless QEMU and CI asserts an un-echoable runtime signal for each feature (the
discipline is [ADR-0016](docs/adr/0016-anti-vacuous-ci-gates.md)).

```bash
# The ~60-gate smoke suite: build + boot + assert every feature's runtime signal
make ci-smoke

# Boot the in-kernel test citizen and grep its PASS/FAIL
make test

# Targeted gates (see the Makefile for the full ci-smoke-* list)
make ci-smoke-disk        # persistence: two boots, one disk
make ci-smoke-mcp         # the host MCP toolbox against a live VM
make ci-smoke-society3    # N kernels synchronize one field
```

### Test Requirements
- All new features must include tests
- Test coverage must be > 80%
- All tests must pass before merge
- Performance tests must meet targets

## Pull Request Process

### Before Submitting
1. **Update Documentation** - Ensure docs reflect changes
2. **Add Tests** - Cover new functionality with tests
3. **Check Style** - Run code style checks
4. **Test Locally** - Verify all tests pass
5. **Update Changelog** - Add entry to CHANGELOG.md

### PR Template
Use the provided PR template:
```markdown
## Description
Brief description of changes

## Type of Change
- [ ] Bug fix
- [ ] New feature
- [ ] Breaking change
- [ ] Documentation update

## Testing
- [ ] Unit tests pass
- [ ] Integration tests pass
- [ ] Manual testing completed

## Checklist
- [ ] Code follows style guidelines
- [ ] Self-review completed
- [ ] Documentation updated
- [ ] Tests added/updated
```

### Review Process
1. **Automated Checks** - CI/CD pipeline runs tests
2. **Code Review** - Maintainers review code changes
3. **Security Review** - Security-sensitive changes get extra review
4. **Integration Testing** - Changes tested in integration environment
5. **Approval** - Maintainer approval required for merge

## Issue Guidelines

### Creating Issues
Use appropriate issue templates:
- **Bug Report** - For reporting bugs
- **Feature Request** - For proposing new features
- **Quantum Component** - For quantum-specific issues

### Issue Labels
- `bug` - Bug reports
- `enhancement` - Feature requests
- `documentation` - Documentation issues
- `security` - Security issues
- `performance` - Performance issues
- `quantum` - Quantum-specific issues
- `good first issue` - Good for newcomers
- `help wanted` - Community help needed

### Bug Reports
Include:
- Clear description of the problem
- Steps to reproduce
- Expected vs actual behavior
- System information
- Relevant logs/output

### Feature Requests
Include:
- Problem statement
- Proposed solution
- Alternatives considered
- Additional context

## Security

### Security Policy
- Security vulnerabilities should be reported privately
- Do not open public issues for security problems
- Security team will investigate and respond
- Security patches will be coordinated

### Reporting Security Issues
Email: security@quantumos.dev
Include "Security Vulnerability" in subject line

### Security Development
- All security changes require review
- Use secure coding practices
- Regular security audits
- Dependency vulnerability scanning

## Community

### Communication Channels
- **GitHub Issues** - Bug reports and feature requests
- **GitHub Discussions** - General questions and discussions
- **Documentation** - Technical documentation and guides

### Getting Help
- Check existing issues and discussions
- Read documentation thoroughly
- Ask specific, well-researched questions
- Be patient and respectful

## Recognition

Contributors are recognized in:
- CONTRIBUTORS.md file
- Release notes
- Project documentation

Thank you for contributing to QuantumOS! 🚀

## License

By contributing to QuantumOS, you agree that your contributions will be licensed under the GNU GPL v2 license.
