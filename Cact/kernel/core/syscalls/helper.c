#include "helper.h"
#include "validate.h"
#include "memory.h"
#include "pipe.h"
#include "socket.h"
#include "tcp.h"
#include "udp.h"

extern int tcp_sock_read_ready(int idx);
extern int udp_sock_read_ready(int idx);

// Find a free file descriptor slot (starting from 3) and install the node.
// Increments the VFS refcount on success.
int alloc_fd(vfs_node_t* node) {
    for (int i = 3; i < MAX_FD; i++) {
        if (!current_task->proc->fds->fd_table[i]) {
            current_task->proc->fds->fd_table[i]   = node;
            current_task->proc->fds->fd_offset[i]  = 0;
            current_task->proc->fds->fd_flags[i]   = 0;
            current_task->proc->fds->fd_cloexec[i] = 0;
            open_vfs(node);   // increment refcount
            return i;
        }
    }
    return -1;
}

// Check if a file descriptor has data available for reading (non-blocking).
// Used by select() and poll().
int fd_read_ready(vfs_node_t* node) {
    if (!node) return 0;
    switch (node->type) {
    case VFS_FILE:
    case VFS_DIRECTORY:
    case VFS_CHARDEVICE:
    case VFS_BLOCKDEVICE:
        return 1;   // always ready
    case VFS_PIPE: {
        pipe_t* p = (pipe_t*)node->priv;
        return p && (p->len > 0 || !p->write_open);
    }
    case VFS_SOCKET: {
        ksock_t* ks = ksock_from_node(node);
        if (!ks) return 0;
        if (ks->kind == KS_TCP) {
            return tcp_sock_read_ready(ks->proto_idx);
        }
        if (ks->kind == KS_UDP)
            return udp_sock_read_ready(ks->proto_idx);
        return 0;
    }
    default: return 0;
    }
}

// Check if a file descriptor can accept a write without blocking.
// Used by select() and poll().
int fd_write_ready(vfs_node_t* node) {
    if (!node) return 0;
    switch (node->type) {
    case VFS_FILE:
    case VFS_DIRECTORY:
    case VFS_CHARDEVICE:
    case VFS_BLOCKDEVICE:
        return 1;   // always ready
    case VFS_PIPE: {
        pipe_t* p = (pipe_t*)node->priv;
        return p && (p->len < PIPE_BUF_SIZE) && p->read_open;
    }
    case VFS_SOCKET: {
        ksock_t* ks = ksock_from_node(node);
        if (!ks) return 0;
        if (ks->kind == KS_TCP) {
            tcp_socket_t* s = &tcp_sockets[ks->proto_idx];
            return (s->state == TCP_ESTABLISHED) || (s->state == TCP_CLOSE_WAIT);
        }
        if (ks->kind == KS_UDP) return 1;   // UDP is always writable
        return 0;
    }
    default: return 0;
    }
}

// Deliver a pending signal to a task by constructing a signal frame on its
// user stack and redirecting EIP to the signal handler.
// Called from syscall_handler before returning to userspace.
void deliver_pending_signal(struct task_struct* t, struct syscall_frame* regs) {
    uint32_t deliverable = t->proc->pending_signals & ~t->proc->signal_mask;
    if (!deliverable) return;

    for (int bit = 0; bit < NSIG; bit++) {
        uint32_t mask = (1u << bit);
        if (!(deliverable & mask)) continue;
        uint32_t handler = t->proc->signal_handlers[bit];
        if (handler <= SIG_IGN) continue;         // default or ignore
        if (handler < USER_SPACE_START || handler >= KERNEL_BASE) continue;  // invalid

        uint32_t new_esp = regs->useresp - sizeof(signal_frame_t);

        // Ensure the signal frame fits within the user stack page
        if (new_esp < USER_SPACE_START || new_esp >= KERNEL_BASE) continue;
        uint32_t page_off = new_esp & 0xFFFu;
        if (page_off + sizeof(signal_frame_t) > PAGE_SIZE) continue;

        // Walk the page tables to locate the physical page for the user stack
        uint32_t* pd  = t->page_directory;
        if (!pd) continue;
        uint32_t  pdi = PD_INDEX(new_esp);
        uint32_t  pti = PT_INDEX(new_esp);
        if (!(pd[pdi] & PAGE_PRESENT)) continue;
        uint32_t* pt = (uint32_t*)(pd[pdi] & ~0xFFFu);
        if (!(pt[pti] & PAGE_PRESENT)) continue;

        uint32_t phys_page = pt[pti] & ~0xFFFu;
        signal_frame_t* frame = (signal_frame_t*)(phys_page + page_off);

        // Save the current user context into the signal frame
        frame->ret_addr = t->proc->sigreturn_trampoline;
        frame->signum   = (uint32_t)bit;
        frame->eax      = regs->eax;
        frame->ecx      = regs->ecx;
        frame->edx      = regs->edx;
        frame->ebx      = regs->ebx;
        frame->esp      = regs->useresp;
        frame->ebp      = regs->ebp;
        frame->esi      = regs->esi;
        frame->edi      = regs->edi;
        frame->eip      = regs->eip;
        frame->eflags   = regs->eflags;

        // Redirect execution to the signal handler
        regs->useresp = new_esp;
        regs->eip     = handler;

        t->proc->pending_signals &= ~mask;   // clear this signal
        return;   // deliver at most one signal per syscall return
    }
}

// Convert VFS node type to POSIX stat mode field (upper 4 bits)
uint32_t _vfs_type_to_mode(uint32_t type) {
    switch (type) {
    case VFS_FILE:        return 0x8000;   // S_IFREG
    case VFS_DIRECTORY:   return 0x4000;   // S_IFDIR
    case VFS_CHARDEVICE:  return 0x2000;   // S_IFCHR
    case VFS_BLOCKDEVICE: return 0x6000;   // S_IFBLK
    case VFS_PIPE:        return 0x1000;   // S_IFIFO
    default:              return 0;
    }
}

// Fill the 4-word stat buffer: [inode, mode, size, type]
void _fill_stat(struct vfs_node* node, uint32_t* ubuf) {
    ubuf[0] = node->inode;
    ubuf[1] = _vfs_type_to_mode(node->type);
    ubuf[2] = node->size;
    ubuf[3] = node->type;
}

// Bounded string copy — always null-terminates
void _kstrcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}