//macros, que exponen ciertos features
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h> // Para malloc()
#include <termios.h>
#include <time.h>      // Para status message
#include <unistd.h>

#define MVI_VERSION "0.0.1"
#define MVI_TAB_STOP 8
// Amount of quit presses to force quit without saving
#define MVI_QUIT_TIMES 2
// Define the control key plus q to quit the operation
#define CTRL_KEY(k) ((k) & 0x1f)

// Enum to give int to the keys pressed in the editor
enum editorKey {
  BACKSPACE = 127,
  // Moves cursor left right up down
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  // Moves cursor to edges of the screen fn arrow left or right
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

// Datatype for storing row of text in our editor
// We use typedef to write a little less everytime we want to use erow
typedef struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

// Struct which will contain the state/config of the editor
struct editorConfig {
  // Cursor positions
  int cx, cy;
  // Render horizontal position
  int rx;
  // Row and column offset
  int rowoff;
  int coloff;
  // Terminal screen rows and colums
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  // Flag to check whether the file has been modified
  int dirty;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios orig_termios;
};

struct editorConfig E;

// Prototype
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);

// Prints an error message and exits the program and clears the screen
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  perror(s);
  exit(1);
}
// Restores the state of the terminal
void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;

  // With ECHO disabled it stops the input from showing as typed (Like typing a password on sudo)
  // With ICANON disabled reads byte-by-byte instead by line
  // With ISIG disabled it disables CTRL+C and CTRL+Z
  // With IEXTEN disabled it disables CTRL+V and CTRL+O (on macOS)
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

  // With IXON disabled it disables CTRL+S and CTRL+Q
  // With ICRNL disabled it disables CTRL+M and stops translating the CR (Enter)
  // The rest are consider a "tradition" when trying to enamble raw mode
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);

  // Minimum number of bytes of input needed before read() returns
  // Maximum amount of time to wait before read() can return
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// Waits for keypresses and returns them
int editorReadKey() {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }

  // Check if key pressed had an escape sequence
  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          // Makes it possible to use page up and page down and also addes home key and end key
          switch (seq[1]) {
            case '1': return HOME_KEY;
            case '3': return DEL_KEY;
            case '4': return END_KEY;
            case '5': return PAGE_UP;
            case '6': return PAGE_DOWN;
            case '7': return HOME_KEY;
            case '8': return END_KEY;
          }
        }
      } else {
        // Makes it possible to use the arrow keys to move and not only wasd also added home key and end key
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
}

// Used to get the cursor position in case which we will use to get the terminal window screen size
int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
    if (buf[i] == 'R') break;
    i++;
  }
  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

// Will place the number of colums and rows of the windows size on the struct winsize if it fails it will return -1
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

// Calculate render x (E.rx)
// Converts index from cx (cursor) to rx (row). Loops through all characters at cx 
// and computes the space of each tab
int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (MVI_TAB_STOP - 1) - (rx % MVI_TAB_STOP);
    rx++;
  }
  return rx;
}

// Uses chars from a string in a erow to fill the render string
// Loops through all row characters and counts the tabs to compute the size
// (Each tab is 8 chars)
// Then, if the char is a tab it add spaces until tab's stop (a constant)
void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;

  free(row->render);
  row->render = malloc(row->size + tabs*(MVI_TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % MVI_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}

// Creates space for a new row and copies the string at the end.
// Multiplies the bits to perform a realloc
void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows) return;
  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

// Frees up the memory of a given row
void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
}

void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows) return;
  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

// Inserts a character into an erow at a given position
void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size) at = row->size;
  row->chars = realloc(row->chars, row->size + 2);
  // memmove is like memcpy, but safe to use when source and destination overlaps
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

// Inserts new line at start or middle of row
void editorInsertNewline() {
  // At start just adds a new row
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    // Breaks the row at cursor position
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.cy++;
  E.cx = 0;
}

// Appends a row to the row before it (also modifying the size)
void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

// Deletes by moving the characters and overwriting the deleted char including the null byte at the end
void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size) return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  // Decrements row size and increment dirtiness
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

// Reads the character and calls editorRowInsertChar and passes cursor position
void editorInsertChar(int c) {
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

// Reads the at cursor position and deletes it if any and moves the cursor
void editorDelChar() {
  if (E.cy == E.numrows) return;
  if (E.cx == 0 && E.cy == 0) return;

  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

// Converts array for erow struct into a string
char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;

  // Lengths of each erow + new line character
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;
  
  *buflen = totlen;
  char *buf = malloc(totlen);
  char *p = buf;

  // Copy the contents of each row at the end of the buffer appending a new line
  for (j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  return buf;
}

// Reads the disk (a file)
void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    while (linelen > 0 && (line[linelen - 1] == '\n' ||
                           line[linelen - 1] == '\r'))
      linelen--;
    editorInsertRow(E.numrows, line, linelen);
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

// Saves the open file
void editorSave() {
  // When file has no name execute prompt to read user input
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)");
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }
  
  int len;
  char *buf = editorRowsToString(&len);

  // Create new file if doesn't exist and open with R&W permissions
  // 0644 is the standard permission for R&W
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);

  if (fd != -1) {
    // Sets the file’s size to the specified length
    // If larger truncates any data at the end
    // If shorter will add padding
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }
  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

// We need to make a buffer for the text that is being written so we dont do small writes but one big write
struct abuf {
  char *b;
  int len;
};

// The append buffer consists of a pointer to the buffer and the lenght of it
#define ABUF_INIT {NULL, 0}

// Appends strings to the buffer it works by getting a memory block of the size
// of the string plus the size that we are adding
void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) return;
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

// Frees the block of memory of a buffer
void abFree(struct abuf *ab) {
  free(ab->b);
}

// Sets the value of row offset so that the cursor is inside the visible window
// will be called at the start of refresh screen
void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }
  
  // Vertical scrolling
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  // Horizontal scrolling
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

// Draws '~' on every row when the editor is called
// It also displays the name and version of the mini vim centered 1/3 down on the terminal screen
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    // rowoff gets a specific row
    // To get the row we want to display:
    // E.rowoff + i
    int filerow = y + E.rowoff;
    // Checks if we are at or after the text buffer
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
          "Kilo editor -- version %s", MVI_VERSION);
        if (welcomelen > E.screencols) welcomelen = E.screencols;
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }
    // Erases the whole line with help of the k command erase in line
    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

// Shows information of the file and line the cursor is at
void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
    E.filename ? E.filename : "[No Name]", E.numrows,
    E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
    E.cy + 1, E.numrows);
  if (len > E.screencols) len = E.screencols;
  abAppend(ab, status, len);
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

// Clears the screen when we render the editors screen
// The second write function is to reposition the cursor to the top left of the editor
void editorRefreshScreen() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
                                            (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

// Sets the message for the status bar
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

// Gets user input (used at saving files without an initial name)
char *editorPrompt(char *prompt) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);
  size_t buflen = 0;

  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();
    
    int c = editorReadKey();
    // Deletes character
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      if (buflen != 0) buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      // Cancels prompts
      editorSetStatusMessage("");
      free(buf);
      return NULL;
    } else if (c == '\r') {
      // Saves buffer (user input)
      if (buflen != 0) {
        editorSetStatusMessage("");
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      // Allocates more memory if it has reached max capacity and adds '\0' end char
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
  }
}

// Process the cursor movement we will move with the wasd keys
void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if (E.cx != 0) {
        E.cx--;
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if (row && E.cx < row->size) {
        E.cx++;
      } else if (row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if (E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if (E.cy < E.numrows) {
        E.cy++;
      }
      break;
  }

  // Keeps the cursor inside the limits of the file
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void quitProgram() {
  //Uses the escape command to clear the screen
  write(STDOUT_FILENO, "\x1b[2J", 4);
  //puts the cursor on 
  write(STDOUT_FILENO, "\x1b[H", 3);
  exit(0);
}

void comandoPuntos(){
  int isave = 0;
  char *opcion = editorPrompt(":%s");
  if (opcion) {
    if (strcmp(opcion, "q") == 0) {
      printf("Quieres guardar tus cambios?\n 1 = si, 0 = no\n");
      scanf("%d", &isave);
      if (isave == 1)
      {
        editorSave();
        quitProgram();
      }else
      {
        quitProgram();
      }
    } else if (strcmp(opcion, "wq") == 0) {
      editorSave();
      quitProgram();
    }
    free(opcion);
  }

}

// Waits for a keypress and then handles the key press its used to map different key 
// combinations and special keys to different functions of the vim, and also to insert and 
// printable keys to the edited text
void editorProcessKeypress() {
  static int quit_times = MVI_QUIT_TIMES;

  int c = editorReadKey();

  switch (c) {
    case ':':
      comandoPuntos();
    break;
    // Enter (CR)
    case '\r':
      editorInsertNewline();
      break;
    // Quit
    case CTRL_KEY('q'):
    if (E.dirty && quit_times > 0) {
        editorSetStatusMessage("Oops... File has unsaved changes. You better save 'em "
          "Press Ctrl-Q %d more times to quit.", quit_times);
        quit_times--;
        return;
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
      exit(0);
      break;

    case CTRL_KEY('s'):
      editorSave();
      break;

    case HOME_KEY:
      E.cx = 0;
      break;

    case END_KEY:
      if (E.cy < E.numrows)
        E.cx = E.row[E.cy].size;
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      // Deletes the character to the right
      if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
      // Deletes the character at cursor
      editorDelChar();
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if (c == PAGE_UP) {
          E.cy = E.rowoff;
        } else if (c == PAGE_DOWN) {
          E.cy = E.rowoff + E.screenrows - 1;
          if (E.cy > E.numrows) E.cy = E.numrows;
        }

        int times = E.screenrows;
        while (times--)
          editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
      }
      break;

    // Move cursor
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
      editorMoveCursor(c);
      break;

    case CTRL_KEY('l'):
    case '\x1b':
      break;
    
    default:
      editorInsertChar(c);
      break;
  }
  quit_times = MVI_QUIT_TIMES;
}

/*** init ***/

void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
  E.screenrows -= 2; // So the status bar has 2 rows
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    // Open editor with file name
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return 0;
}
