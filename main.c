//macros, que exponen ciertos features
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/types.h> //para malloc()
#include <time.h> //para status message
#include <string.h>


//Define the control key plus q to quit the operation
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define CTRL_KEY(k) ((k) & 0x1f)

//enumarator to give int to the keys pressed in the editor
enum editorKey{
  //move cursor left right up down
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  //move cursor to edges of the screen fn arrow left or right
  HOME_KEY,
  END_KEY,
  //move cursor top or botton fn arrow up or down
  PAGE_UP,
  PAGE_DOWN

};

//Datatype for storing row of text in our editor
//usamos typedef para no tener que especificar siempre struct erow
//
typedef struct erow {
  int size;
  char *chars;
  //render vars
  int rsize;
  char *render;
} erow;

//struct which wil contain the state of the editor
struct editorConfig
{
  //cursor positions
  int cx,cy;
  //render horizontal position
  int rx;
  //row and column offset
  int rowoff;
  int coloff;
  //terminal screen rows and colums
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios orig_termios;
};

struct editorConfig E;


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
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcgetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;

  // With ECHO disabled it stops the input from showing as typed (Like typing a password on sudo)
  // With ICANON disabled reads byte-by-byte instead by line
  // With ISIG disabled it disables CTRL+C and CTRL+Z
  // With IEXTEN disabled it disables CTRL+V and CTRL+O (on macOS)
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

  // With IXON disabled it disables CTRL+S and CTRL+Q
  // With ICRNL disabled it disables CTRL+M and stops translating the CR (Enter)
  // The rest are consider a "tradition" when trying to enamble raw mode
  raw.c_iflag &= ~(IXON | ICRNL | INPCK | ISTRIP | BRKINT);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);

  // Minimum number of bytes of input needed before read() returns
  // Maximum amount of time to wait before read() can return
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    die("tcsetattr");
}

//this function waits for keypresses and returns them
int editorReadKey(){
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) die("read");
  }
  //check if key pressed had an escape sequence
  if (c == '\x1b') {
    char seq[3];
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
        if (seq[2] == '~') {
          //make it possible to use page up and page down and also addes home key and end key
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
      }else{
        //make it possible to use the arrow keys to move and not only wasd also added home key and end key
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
        }
      }
    }else if (seq[0] == 'O')
    {
      switch (seq[1]) {
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
      }
    }
    return '\x1b';
  }else{
    return c;
  }
}

//this function is used to get the cursor position in case which we will use to get the terminal window screen size
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

  if (buf[0] != '\x1b' || buf[1] != '['){ 
    return -1;
  }
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2){
    return -1;
  }
  return 0;
}

//this function will place the number of colums and rows of the windows size on the struct winsize if it fails it will return -1
int getWindowSize(int *rows, int *cols){
  struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
    return getCursorPosition(rows, cols);
  }else{
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

//funcion que calcula render x (E.rx)
//en resumen, convierte index de cx a rx. loopea todos los characters de cx, y calcula los espacios de cada tab
int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    if (row->chars[j] == '\t')
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
    rx++;
  }
  return rx;
}

//funcion que usa los chars de un string en un erow para llenar el render string
//loopea los chars del row y cuenta los tabs para saber el tamano a desplegar
//cada tab son 8 characters. Esto es porque el tab ocupa mas espacio de lo que parece
//luego un for para checar si estamos en un tab. Si si, le ponemos espacios hasta stop del tab
//stop del tab es una constante
void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++)
    if (row->chars[j] == '\t') tabs++;

  free(row->render);
  row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}


//da espacio para una nueva erow, y copia el string al final de e.row
//multiplicamos los bits para que realloc sepa cuantos bytes usar
void editorAppendRow(char *s, size_t len) {
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  
  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
}

//sera para leer del disco
void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);
  FILE *fp = fopen(filename, "r");
  if (!fp) die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen ;
  linelen = getline(&line, &linecap, fp);
  while ((linelen = getline(&line, &linecap, fp)) != -1) { //con esta linea leemos todas las lineas del file
    while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
        linelen--;

    editorAppendRow(line,linelen);
  }
  free(line);
  fclose(fp);

}

//we need to make a buffer for the text that is being written so we dont do small writes but one big write
struct abuf
{
  char *b;
  int len;
};
//the append buffer consists of a pointer to the buffer and the lenght of it
#define ABUF_INIT {NULL, 0};

//this function is used to append strings to the buffer it works by getting a memory block of the size of the string plus the size that we are adding 
void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL ){
    return;
  }
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

//frees the block of memory of a buffer
void abFree(struct abuf *ab){
  free(ab->b);
}

//function to process the cursor movement we will move with the wasd keys
void editorMoveCursor(int key){
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key)
  {
    case ARROW_LEFT:
      if(E.cx != 0){
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
    if(E.cy != 0){
      E.cy--;
    }
    break;
    case ARROW_DOWN:
    if (E.cy < E.numrows) {
      E.cy++;
    }
    break;
  }
  //para mantener el cursor dentro de los limites del archivo
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

//waits for a keypress and then handles the key press its used to map different key combinations and special keys to different functions of the vim, and also to insert andy printable keys to the edited text
void editorProcessKeypress(){
  int c = editorReadKey();
  switch (c)
  {
    //quit
  case CTRL_KEY('q'):
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
      case HOME_KEY:
        E.cx = 0;
      break;
      case END_KEY:
        if (E.cy < E.numrows){
        E.cx = E.row[E.cy].size;
        }
      break;
    //page up and page down
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
    //move cursor
    case ARROW_UP:
          editorMoveCursor(c);
    break;
    case ARROW_DOWN:
          editorMoveCursor(c);
    break;
    case ARROW_LEFT:
          editorMoveCursor(c);
    break;
    case ARROW_RIGHT:
      editorMoveCursor(c);
    break;
  }
}

//Sets the value of row offset so that the cursor is inside the visible window
//will be called at the start of refresh screen

//tiene un if, donde checa si esta por en cima de la window
//el segundo if, es si se pasa del fondo de la window
void editorScroll() {
  //init para render x
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  //vertical scrolling
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  //horizontal scrolling
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

//this function draws ~ on every row when the editor is called
//it also fisplays the name and version of the mini vim centered 1/3 down on the terminal screen
void editorDrawRows(struct abuf *ab){
  int i;
  for (i = 0; i < E.screenrows; i++){
    //rowoff nos permite accesar una row especifica
    //PARA ACCESAR LA FILA QUE QUEREMOS DESPLEGAR:
    //E.rowoff + i
    int filerow = i + E.rowoff;
    //If que checa si estamos en el text buffer, o despues del text buffer
    if (filerow >= E.numrows){
      if (i == E.screenrows / 3 && E.numrows == 0) { //solo se despliega si esta vacio
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
          "Mini-Vim -- version %s", KILO_VERSION);
        if (welcomelen > E.screencols){
          welcomelen = E.screencols;
        }
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while (padding--) abAppend(ab, " ", 1);
        abAppend(ab, welcome, welcomelen);
      }else{
        abAppend(ab, "~", 1);
      }
    }else{
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0) len = 0;
      if (len > E.screencols) len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }
    //this erases the whole line with help of the k command erase in line
    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
    }
  
  
}

//para hacer que el status bar sea invertido, y muestre info del archivo
//tambien muestra informacion de la linea en donde estamos
void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines",
    E.filename ? E.filename : "[No Name]", E.numrows);
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
  abAppend(ab, "\r\n", 2);//para linea 1
}

//Funcion que le dice al message que tener
void editorDrawMessageBar(struct abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols) msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

//this function clears the screen when we render the editors screen
//the second write function is to reposition the cursor to the top left of the editor
void editorRefreshScreen(){
  editorScroll();

  struct abuf ab = ABUF_INIT;
  //hides the cursor
  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);
  
  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  //cursor position
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
                                            (E.rx - E.coloff) + 1); 
  abAppend(&ab, buf, strlen(buf));

  //shows the cursor
  abAppend(&ab, "\x1b[?25l", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

//funcion que pone el statusmessage, basado en tiempo
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

void initEditor(){
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.filename = NULL;

  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1){
    die("getWindowSize");
  }
  E.screenrows -= 2; //eso para que el status bar tenga 2 rows
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]); //editorOpen ahora toma nobmbre de archivo
  }
  //status message inicial
  editorSetStatusMessage("HELP: Ctrl-Q = quit");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }
  return 0;
}