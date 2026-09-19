#ifndef PTY_H
#define PTY_H

#include <stdint.h>

/* Pseudo-terminal pairs behind /dev/ptmx + /dev/pts/<n>.
 *
 * Opening /dev/ptmx allocates a pair and yields the master end; the matching
 * slave is /dev/pts/<n>.  The slave stays locked until the master unlocks it
 * (TIOCSPTLCK), which is the Linux handshake that lets the opener configure
 * the terminal before anyone else can open it. */

#define PTY_MAX  16
#define PTY_BUF  4096

int  pty_alloc(void);                    /* allocate a pair, or -1 */
int  pty_used(int i);                    /* is pair i allocated? */
int  pty_unlocked(int i);                /* slave openable? */
void pty_lock(int i, int locked);

int  pty_master_read (int i, uint32_t size, char *buf);
int  pty_master_write(int i, uint32_t size, char *buf);
int  pty_slave_read  (int i, uint32_t size, char *buf);
int  pty_slave_write (int i, uint32_t size, char *buf);

void pty_close_master(int i);
void pty_close_slave (int i);
void pty_slave_opened(int i);

#endif
