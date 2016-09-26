#define MAXGAMESIZE 65536 // number of bytes we reasonably expect every game to be smaller than

#define UTF8_LEFT_DBLQUOTE 147
#define UTF8_RIGHT_DBLQUOTE 148
#define UTF8_NOBREAK_SPACE 160

typedef enum {
	PDN_IDLE, PDN_READING_FROM, PDN_WAITING_SEP, PDN_WAITING_TO, PDN_READING_TO, PDN_WAITING_OPTIONAL_TO,
	PDN_WAITING_OPTIONAL_SEP, PDN_CURLY_COMMENT, PDN_NEMESIS_COMMENT, PDN_FLUFF, PDN_QUOTED_VALUE, PDN_DONE
} PDN_PARSE_STATE;

int PDNparseGetnextgame(char **start,char *game);		/* gets whats between **start and game terminator */
int PDNparseGetnextheader(char **start,char *header);/* gets whats betweeen [] from **start */
int PDNparseGetnexttag(char **start,char *tag);		/* gets whats between "" from **start */
int PDNparseTokentonumbers(char *token,int *from, int *to);
int PDNparseGetnexttoken(char **start, char *token);	/* gets the next token from **start */
int PDNparseGetnextPDNtoken(char ** start, char *token);
int PDNparseGetnumberofgames(char *filename);			/* tokens are: -> {everything in a comment}*/
size_t getfilesize(char *filename);							//-> a move: "11-15" or ""4x12" -> a text: "event"*/

