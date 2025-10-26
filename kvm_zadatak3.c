// (cela datoteka — zameni svoj kvm_zadatak3.c ovim sadržajem)
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <linux/kvm.h>
#include <stdbool.h>
#include <pthread.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>

#define GUEST_START_ADDR 0x0000

#define PDE64_PRESENT (1u << 0)
#define PDE64_RW (1u << 1)
#define PDE64_USER (1u << 2)
#define PDE64_PS (1u << 7)

#define CR0_PE (1u << 0)
#define CR0_PG (1u << 31)
#define CR4_PAE (1u << 5)
#define EFER_LME (1u << 8)
#define EFER_LMA (1u << 10)

// File I/O port protocol
#define FILE_PORT 0x0278
#define FILE_OPEN  1
#define FILE_CLOSE 2
#define FILE_READ  3
#define FILE_WRITE 4
#define FILE_OPEN_READONLY 5

struct vm {
	int kvm_fd;
	int vm_fd;
	int vcpu_fd;
	char *mem;
	size_t mem_size;
	struct kvm_run *run;
	int run_mmap_size;
};

typedef struct {
	int guest_fd;
	int host_fd;
	char path[256];
	bool shared_copy;
} FileDescriptor;

typedef struct {
	FileDescriptor fds[32];
	int fd_count;
	char shared_files[32][256];
	int shared_count;
	int vm_id;

	// IO accumulate state
	uint8_t pending_op;       // 0 == none, otherwise FILE_*
	uint8_t io_buf[4096];     // accumulator for incoming bytes for current op
	int io_buf_len;

	uint8_t output;
	
} FileContext;

typedef struct {
	int size;
	bool cetriKB;
	char* img;
	char shared_files[32][256];
	int shared_count;
	int vm_id;
} argumenti;

int vm_init(struct vm *v, size_t mem_size)
{
	struct kvm_userspace_memory_region region;	

	memset(v, 0, sizeof(*v));
	v->mem_size = mem_size;

	v->kvm_fd = open("/dev/kvm", O_RDWR);
	if (v->kvm_fd < 0) {
		perror("open /dev/kvm");
		return -1;
	}

    int api = ioctl(v->kvm_fd, KVM_GET_API_VERSION, 0);
    if (api != KVM_API_VERSION) {
        fprintf(stderr, "KVM API mismatch: kernel=%d headers=%d\n", api, KVM_API_VERSION);
        return -1;
    }

	v->vm_fd = ioctl(v->kvm_fd, KVM_CREATE_VM, 0);
	if (v->vm_fd < 0) {
		perror("KVM_CREATE_VM");
		return -1;
	}

	v->mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (v->mem == MAP_FAILED) {
		perror("mmap mem");
		return -1;
	}

	region.slot = 0;
	region.flags = 0;
	region.guest_phys_addr = 0;
	region.memory_size = mem_size;
	region.userspace_addr = (uintptr_t)v->mem;
    if (ioctl(v->vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
		perror("KVM_SET_USER_MEMORY_REGION");
        return -1;
	}

	v->vcpu_fd = ioctl(v->vm_fd, KVM_CREATE_VCPU, 0);
    if (v->vcpu_fd < 0) {
		perror("KVM_CREATE_VCPU");
        return -1;
	}

	v->run_mmap_size = ioctl(v->kvm_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (v->run_mmap_size <= 0) {
		perror("KVM_GET_VCPU_MMAP_SIZE");
		return -1;
	}

	v->run = mmap(NULL, v->run_mmap_size, PROT_READ | PROT_WRITE,
			     MAP_SHARED, v->vcpu_fd, 0);
	if (v->run == MAP_FAILED) {
		perror("mmap kvm_run");
		return -1;
	}

	return 0;
}

void vm_destroy(struct vm *v) {
	if (v->run && v->run != MAP_FAILED)
		munmap(v->run, (size_t)v->run_mmap_size);
	if (v->mem && v->mem != MAP_FAILED)
		munmap(v->mem, v->mem_size);
	if (v->vcpu_fd >= 0)
		close(v->vcpu_fd);
	if (v->vm_fd >= 0)
		close(v->vm_fd);
	if (v->kvm_fd >= 0)
		close(v->kvm_fd);
}

static void setup_segments_64(struct kvm_sregs *sregs)
{
	struct kvm_segment code = {
		.base = 0,
		.limit = 0xffffffff,
		.present = 1,
		.type = 11,
		.dpl = 0,
		.db = 0,
		.s = 1,
		.l = 1,
		.g = 1,
	};
	struct kvm_segment data = code;
	data.type = 3;
	data.l = 0;

	sregs->cs = code;
	sregs->ds = sregs->es = sregs->fs = sregs->gs = sregs->ss = data;
}

static void setup_long_mode(struct vm *v, struct kvm_sregs *sregs, bool cetriKB)
{
	uint64_t pml4_addr = 0x1000;
	uint64_t *pml4 = (void *)(v->mem + pml4_addr);

	uint64_t pdpt_addr = 0x2000;
	uint64_t *pdpt = (void *)(v->mem + pdpt_addr);

	uint64_t pd_addr = 0x3000;
	uint64_t *pd = (void *)(v->mem + pd_addr);

	uint64_t pt_addr = 0x4000;
	uint64_t *pt = (void *)(v->mem + pt_addr);

	memset(pml4, 0, 4096);
	memset(pdpt, 0, 4096);
	memset(pd, 0, 4096);
	memset(pt, 0, 4096);

	pml4[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pdpt_addr;
	pdpt[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pd_addr;

	if (cetriKB)
	{
		pd[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pt_addr;
		uint64_t page = 0;
		for(int i = 0; i < 512; i++) {
			pt[i] = page | PDE64_PRESENT | PDE64_RW | PDE64_USER;
			page += 0x1000;
		}
	}
	else
	{
		// 2MB pages
		pd[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | PDE64_PS | 0; // maps first 2MB
	}

	sregs->cr3  = pml4_addr;
	sregs->cr4  = CR4_PAE;
	sregs->cr0  = CR0_PE | CR0_PG;
	sregs->efer = EFER_LME | EFER_LMA;
	setup_segments_64(sregs);
}

int load_guest_image(struct vm *v, const char *image_path, uint64_t load_addr) {
    FILE *f = fopen(image_path, "rb");
    if (!f) {
        perror("Failed to open guest image");
        return -1;
    }

    if (fseek(f, 0, SEEK_END) < 0) {
        perror("Failed to seek to end of guest image");
        fclose(f);
        return -1;
    }

    long fsz = ftell(f);
    if (fsz < 0) {
        perror("Failed to get size of guest image");
        fclose(f);
        return -1;
    }
    rewind(f);

    printf("[VM] Loading guest image '%s' size = %ld bytes\n", image_path, fsz);
    printf("[VM] VM memory size = %zu bytes, load_addr = 0x%llx\n",
           v->mem_size, (unsigned long long)load_addr);

    if ((uint64_t)fsz > v->mem_size - load_addr) {
        fprintf(stderr, "Guest image is too large (fsz=%ld > mem_size-load_addr=%zu)\n",
                fsz, v->mem_size - load_addr);
        fclose(f);
        return -1;
    }

    size_t got = fread((uint8_t*)v->mem + load_addr, 1, (size_t)fsz, f);
    if (got != (size_t)fsz) {
        fprintf(stderr, "Failed to read full guest image: got %zu, expected %ld\n", got, fsz);
        fclose(f);
        return -1;
    }

    fclose(f);
    printf("[VM] Guest image loaded successfully\n");
    return 0;
}

// File utilities

bool is_shared(FileContext *ctx, const char *path) {
	for (int i = 0; i < ctx->shared_count; i++) {
		if (strcmp(ctx->shared_files[i], path) == 0)
			return true;
	}
	return false;
}

int get_host_fd(FileContext *ctx, int guest_fd) {
	for (int i = 0; i < ctx->fd_count; i++)
		if (ctx->fds[i].guest_fd == guest_fd)
			return ctx->fds[i].host_fd;
	return -1;
}

void remove_fd(FileContext *ctx, int guest_fd) {
	for (int i = 0; i < ctx->fd_count; i++) {
		if (ctx->fds[i].guest_fd == guest_fd) {
			if (ctx->fds[i].host_fd >= 0) close(ctx->fds[i].host_fd);
			// shift remaining
			for (int j = i; j < ctx->fd_count - 1; j++)
				ctx->fds[j] = ctx->fds[j+1];
			ctx->fd_count--;
			return;
		}
	}
}

int open_file(FileContext *ctx, const char *path, const char *mode)
{
    char local_path[256];
    strcpy(local_path, path);
    bool shared = is_shared(ctx, path);

    // If shared and opening for writing -> create per-VM local copy name
    if (shared && (strchr(mode, 'w') || strchr(mode, '+'))) {
        const char *basename = strrchr(path, '/');
        if (basename) basename++;
        else basename = path;
        snprintf(local_path, sizeof(local_path),
                 "vm%d_%s", ctx->vm_id, basename);
        printf("[VM%d] Creating local copy  %s\n", ctx->vm_id, local_path);
        
        // Copy content from original file to local copy
        FILE *src = fopen(path, "r+");
        if (src && access(local_path, F_OK) != 0) { // only if local copy doesn't exist
            FILE *dst = fopen(local_path, "w");
            if (dst) {
                char buffer[4096];
                size_t bytes;
                while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
                    fwrite(buffer, 1, bytes, dst);
                }
                fclose(dst);
            }
            fclose(src);
        }
         
        // override mode to r+ so it opens the copy for reading and writing
        
    }

	if(access(local_path, F_OK) != 0 ) {
		// Create the file if it doesn't exist
		FILE *new_file = fopen(local_path, "w");
		if (new_file) {
			fclose(new_file);
		}
	}

    FILE *f = fopen(local_path, "r+");
    if (!f) {
        perror("fopen");
        return -1;
    }
	
	lseek(fileno(f), 0, SEEK_SET);
    int guest_fd = ctx->fd_count + 3; // first guest FD is 3
    ctx->fds[ctx->fd_count].guest_fd = guest_fd;
    ctx->fds[ctx->fd_count].host_fd = fileno(f);
    strcpy(ctx->fds[ctx->fd_count].path, local_path);
    ctx->fds[ctx->fd_count].shared_copy = shared;
    ctx->fd_count++;
    printf("[VM%d] OPEN %s -> fd=%d\n", ctx->vm_id, local_path, guest_fd);
    return guest_fd;
}

int open_file_READONLY(FileContext *ctx, const char *path)
{
    FILE *f = fopen(path, "r+");
    if (!f) {
        perror("fopen");
        return -1;
    }
	
	lseek(fileno(f), 0, SEEK_SET);
    int guest_fd = ctx->fd_count + 3; // first guest FD is 3
    ctx->fds[ctx->fd_count].guest_fd = guest_fd;
    ctx->fds[ctx->fd_count].host_fd = fileno(f);
    strcpy(ctx->fds[ctx->fd_count].path, path);
    ctx->fds[ctx->fd_count].shared_copy = true;
    ctx->fd_count++;
    printf("[VM%d] OPEN %s -> fd=%d\n", ctx->vm_id, path, guest_fd);
    return guest_fd;
}


// Parse/handle accumulated IO buffer for FileContext
static void try_process_io_buffer(FileContext *ctx)
{
	// parser works on ctx->io_buf[0..io_buf_len-1]
	while (ctx->io_buf_len > 0) {
		if (ctx->pending_op == FILE_OPEN) {
			// look for null terminator
			int found = -1;
			for (int i = 0; i < ctx->io_buf_len; i++) {
				if (ctx->io_buf[i] == 0) { found = i; break; }
			}
			if (found == -1) return; // wait for more bytes
			// we have full path
			char path[256];
			int copy_len = (found < 255) ? found : 255;
			memcpy(path, ctx->io_buf, copy_len);
			path[copy_len] = 0;
			int fd = open_file(ctx, path, "r+");
			
			ctx->output = (fd >= 0) ? fd : 0xFF; // Store fd in output for guest to read
			// remove consumed bytes (path + null)
			int consume = found + 1;
			memmove(ctx->io_buf, ctx->io_buf + consume, ctx->io_buf_len - consume);
			ctx->io_buf_len -= consume;
			ctx->pending_op = 0;
			continue;
		}
		else if (ctx->pending_op == FILE_OPEN_READONLY) {
			// look for null terminator
			int found = -1;
			for (int i = 0; i < ctx->io_buf_len; i++) {
				if (ctx->io_buf[i] == 0) { found = i; break; }
			}
			if (found == -1) return; // wait for more bytes
			// we have full path
			char path[256];
			int copy_len = (found < 255) ? found : 255;
			memcpy(path, ctx->io_buf, copy_len);
			path[copy_len] = 0;
			int fd = open_file_READONLY(ctx, path);
			
			ctx->output = (fd >= 0) ? fd : 0xFF; // Store fd in output for guest to read
			// remove consumed bytes (path + null)
			int consume = found + 1;
			memmove(ctx->io_buf, ctx->io_buf + consume, ctx->io_buf_len - consume);
			ctx->io_buf_len -= consume;
			ctx->pending_op = 0;
			continue;
		}
		else if (ctx->pending_op == FILE_CLOSE) {
			// need 4 bytes (int)
			if (ctx->io_buf_len < 4) return;
			int guest_fd;
			memcpy(&guest_fd, ctx->io_buf, 4);
			remove_fd(ctx, guest_fd);
			printf("[VM%d] CLOSE fd=%d\n", ctx->vm_id, guest_fd);
			// remove consumed
			memmove(ctx->io_buf, ctx->io_buf + 4, ctx->io_buf_len - 4);
			ctx->io_buf_len -= 4;
			ctx->pending_op = 0;
			continue;
		}
		else if (ctx->pending_op == FILE_WRITE) {
			// need header: guest_fd (4) + len (4)
			if (ctx->io_buf_len < 8) return;
			int guest_fd;
			int len;
			memcpy(&guest_fd, ctx->io_buf, 4);
			memcpy(&len, ctx->io_buf + 4, 4);
			if (len < 0 || len > 2000000) { // sanity
				fprintf(stderr, "[VM%d] Invalid write len=%d\n", ctx->vm_id, len);
				// abort op
				ctx->io_buf_len = 0;
				ctx->pending_op = 0;
				return;
			}
			if (ctx->io_buf_len < 8 + len) return; // wait for data
			int host_fd = get_host_fd(ctx, guest_fd);
			if (host_fd >= 0) {
				// Seek to end of file before writing (append)
				lseek(host_fd, 0, SEEK_END);
				ssize_t wrote = write(host_fd, ctx->io_buf + 8, len);
				(void)wrote;
			}
			printf("[VM%d] WRITE fd=%d (%d bytes)\n", ctx->vm_id, guest_fd, len);
			// consume
			int consume = 8 + len;
			memmove(ctx->io_buf, ctx->io_buf + consume, ctx->io_buf_len - consume);
			ctx->io_buf_len -= consume;
			ctx->pending_op = 0;
			continue;
		}
		else if (ctx->pending_op == FILE_READ) {
			// need header guest_fd (4) only - reading 1 byte at a time
			if (ctx->io_buf_len < 4) return;
			
			int guest_fd;
			memcpy(&guest_fd, ctx->io_buf, 4);
			int host_fd = get_host_fd(ctx, guest_fd);
			if (host_fd >= 0) {
				// Seek to beginning of file before reading
				//lseek(host_fd, 0, SEEK_SET);
				char byte;
				int r = read(host_fd, &byte, 1);
				if (r == 1) {
					ctx->output = (uint8_t)byte;
					printf("[VM%d] READ fd=%d -> 1 byte: 0x%02x\n", ctx->vm_id, guest_fd, ctx->output);
				} else {
					ctx->output = 0;
					printf("[VM%d] READ fd=%d -> no data, r = %d, hostfd = %d, questfd = %d, byte = %c\n", ctx->vm_id, guest_fd, r, host_fd, guest_fd, (char)byte);
				}
			} else {
				ctx->output = 0;
				printf("[VM%d] READ fd=%d -> invalid fd\n", ctx->vm_id, guest_fd);
			}
			// consume header
			memmove(ctx->io_buf, ctx->io_buf + 4, ctx->io_buf_len - 4);
			ctx->io_buf_len -= 4;
			ctx->pending_op = 0;
			continue;
		}
		else {
			// no pending op but buffer has data - shouldn't happen, reset
			ctx->io_buf_len = 0;
			ctx->pending_op = 0;
			return;
		}
	}
}

// VM thread function
void* glavnaFunkcija(void* args)
{
	argumenti* myArgs = (argumenti*) args;
	struct vm v;
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	int stop = 0;
	int ret = 0;

	FileContext file_ctx;
	memset(&file_ctx, 0, sizeof(file_ctx));
	file_ctx.vm_id = myArgs->vm_id;
	file_ctx.shared_count = myArgs->shared_count;
	for (int i = 0; i < myArgs->shared_count; i++)
		strcpy(file_ctx.shared_files[i], myArgs->shared_files[i]);

	if (vm_init(&v, myArgs->size)) {
		fprintf(stderr, "[VM%d] Failed to init VM\n", myArgs->vm_id);
		free(myArgs);
		return NULL;
	}

	if (ioctl(v.vcpu_fd, KVM_GET_SREGS, &sregs) < 0) {
		perror("KVM_GET_SREGS");
		vm_destroy(&v);
		free(myArgs);
		return NULL;
	}
	setup_long_mode(&v, &sregs, myArgs->cetriKB);
	if (ioctl(v.vcpu_fd, KVM_SET_SREGS, &sregs) < 0) {
		perror("KVM_SET_SREGS");
		vm_destroy(&v);
		free(myArgs);
		return NULL;
	}

	if (load_guest_image(&v, myArgs->img, GUEST_START_ADDR) < 0) {
		vm_destroy(&v);
		free(myArgs);
		return NULL;
	}

	memset(&regs, 0, sizeof(regs));
	regs.rflags = 0x2;
	regs.rip = 0;
	regs.rsp = (2ull << 20); // 2 MB stack
	if (ioctl(v.vcpu_fd, KVM_SET_REGS, &regs) < 0) {
		perror("KVM_SET_REGS");
		vm_destroy(&v);
		free(myArgs);
		return NULL;
	}

	free(myArgs); // no longer needed

	while(!stop) {
		ret = ioctl(v.vcpu_fd, KVM_RUN, 0);
		if (ret == -1) {
			perror("KVM_RUN");
			break;
		}

		switch (v.run->exit_reason) {
			case KVM_EXIT_IO: {
				uint8_t *p = (uint8_t *)v.run + v.run->io.data_offset;
				int bytes = v.run->io.size * v.run->io.count;

				// Serial output (port 0xE9)
				if (v.run->io.direction == KVM_EXIT_IO_OUT &&
				    v.run->io.port == 0xE9) {
					for (int i = 0; i < bytes; i++) {
						putchar(p[i]);
					}
					fflush(stdout);
					continue;
				}
				if (v.run->io.direction == KVM_EXIT_IO_IN && v.run->io.port == 0x0278) {
  					*((char*)v.run + v.run->io.data_offset) = file_ctx.output;
				}
				// File protocol on FILE_PORT
				if (v.run->io.direction == KVM_EXIT_IO_OUT &&
				    v.run->io.port == FILE_PORT) {
					// append data to file_ctx buffer or process op byte
					int idx = 0;
					while (idx < bytes) {
						if (file_ctx.pending_op == 0) {
							// first byte is op
							file_ctx.pending_op = p[idx++];
							file_ctx.io_buf_len = 0; // reset accumulator
						}
						// append remaining bytes to buffer
						int tocopy = bytes - idx;
						if (tocopy > (int)sizeof(file_ctx.io_buf) - file_ctx.io_buf_len)
							tocopy = (int)sizeof(file_ctx.io_buf) - file_ctx.io_buf_len;
						if (tocopy > 0) {
							memcpy(file_ctx.io_buf + file_ctx.io_buf_len, p + idx, tocopy);
							file_ctx.io_buf_len += tocopy;
							idx += tocopy;
						}
						// try to process (may consume part or all of buffer)
						try_process_io_buffer(&file_ctx);
					}
					continue;
				}

				break;
			}
			case KVM_EXIT_HLT:
				printf("[VM%d] KVM_EXIT_HLT\n", file_ctx.vm_id);
				stop = 1;
				break;
			case KVM_EXIT_SHUTDOWN:
				printf("[VM%d] SHUTDOWN\n", file_ctx.vm_id);
				stop = 1;
				break;
			default:
				printf("[VM%d] Unknown exit reason %d\n", file_ctx.vm_id, v.run->exit_reason);
				stop = 1;
				break;
		}
	}
	vm_destroy(&v);
	return NULL;
}

int main(int argc, char *argv[])
{
	int memory_mb = 2;
	int page_size_kb = 4;
	bool cetriKB = true;

	char* guests[32];
	int guest_count = 0;
	char shared_files[32][256];
	int shared_count = 0;

	static struct option long_options[] = {
		{"memory", required_argument, 0, 'm'},
		{"page", required_argument, 0, 'p'},
		{"guest", required_argument, 0, 'g'},
		{"file", required_argument, 0, 'f'},
		{0,0,0,0}
	};

	int opt, option_index = 0;
	while ((opt = getopt_long(argc, argv, "m:p:g:f:", long_options, &option_index)) != -1) {
		switch (opt) {
			case 'm':
				memory_mb = atoi(optarg);
				if (memory_mb != 2 && memory_mb != 4 && memory_mb != 8) {
					fprintf(stderr, "Error: memory must be 2,4,8\n");
					return 1;
				}
				break;
			case 'p':
				page_size_kb = atoi(optarg);
				cetriKB = (page_size_kb == 4);
				break;
			case 'g':
				// gather one or more guest filenames
				guests[guest_count++] = optarg;
				while (optind < argc && argv[optind][0] != '-') {
					guests[guest_count++] = argv[optind++];
				}
				break;
			case 'f':
				strcpy(shared_files[shared_count++], optarg);
				while (optind < argc && argv[optind][0] != '-') {
					strcpy(shared_files[shared_count++], argv[optind++]);
				}
				break;
			default:
				fprintf(stderr, "Unknown option\n");
				return 1;
		}
	}

	if (guest_count == 0) {
		fprintf(stderr, "No guests provided. Use --guest guest.img\n");
		return 1;
	}

	pthread_t threads[32];
	int size = memory_mb * 1024 * 1024;

	for (int i = 0; i < guest_count; i++) {
		argumenti *args = malloc(sizeof(argumenti));
		memset(args, 0, sizeof(*args));
		args->size = size;
		args->cetriKB = cetriKB;
		args->img = guests[i];
		args->shared_count = shared_count;
		args->vm_id = i;
		for (int j = 0; j < shared_count; j++)
			strcpy(args->shared_files[j], shared_files[j]);

		int rc = pthread_create(&threads[i], NULL, glavnaFunkcija, args);
		if (rc != 0) {
			fprintf(stderr, "Failed to create thread for guest %s: %s\n", guests[i], strerror(rc));
			free(args);
		}
	}

	for (int i = 0; i < guest_count; i++)
		pthread_join(threads[i], NULL);

	return 0;
}
