#ifdef UNITY_BUILD

unsigned char *ptrbuf;
//unsigned char ptrbuf[200];
int ptrbuflen;
int *ptpbuf;
//int ptpbuf[200];
int ptpbuflen;
 
int ptpfd = -1;
 
int ptrpos;

int tapeUpdated;

const char *hole_vs_src =
glslheader
"VSIN vec2 in_pos;\n"
"VSIN vec2 in_uv;\n"
"VSOUT vec2 v_uv;\n"
"uniform float u_size;\n"
"void main()\n"
"{\n"
"	v_uv = in_uv;\n"
"	vec2 p = vec2(in_pos.x*2.0-1.0, -(in_pos.y*2.0-1.0));\n"
"	gl_Position = vec4(p.x, p.y, -0.5, 1.0);\n"
"	gl_PointSize = 2.0*u_size;\n"
"}\n";

const char *hole_fs_src = 
glslheader
outcolor
"uniform vec4 u_color;\n"
"void main()\n"
"{\n"
"	vec2 pos = 2.0*gl_PointCoord - vec2(1.0);\n"
"	vec4 color = u_color;\n"
"	color.a = 1.0 - smoothstep(0.8, 1.0, length(pos));\n"
output
"}\n";

GLint hole_program;

void
initReader(void)
{
	GLint hole_vs = compileshader(GL_VERTEX_SHADER, hole_vs_src);
	GLint hole_fs = compileshader(GL_FRAGMENT_SHADER, hole_fs_src);
	hole_program = linkprogram(hole_fs, hole_vs);

	glGenBuffers(1, &immVbo);
}

void
drawHole(float x, float y, float r)
{
	x /= drawW;
	y /= drawH;

	Vertex v[] = { { x, y,	0.0f, 0.0f } };
	glUseProgram(hole_program);
	setvbo(immVbo);
	glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);
	glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
	glUniform4f(glGetUniformLocation(hole_program, "u_color"),
		col.r/255.0f, col.g/255.0f, col.b/255.0f, col.a/255.0f);
	glUniform1f(glGetUniformLocation(hole_program, "u_size"), r);
	glDrawArrays(GL_POINTS, 0, 1);
}

void
drawReader(Region *reg)
{
	setRegion(reg);

	int width = drawW;
	int height = drawH;

	int space = height/10;
	if(space < 4) space = 4;
	// heuristics
	int r = space/2 - 1;
	int fr = r/2 + 1;
	if(fr == r) fr--;

	int i = ptrpos - 10;
	int c;


	setColor(80, 80, 80, 255);
	drawRectangle(0.0f, 0.0f, width, height);

	if(ptrbuflen > 0) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		setColor(255, 255, 176, 255);
		int x = i < 0 ? -i*space : 0;
		int w = (ptrbuflen-i)*space;
		drawRectangle(x, 0.0f, w, height);

		setColor(0, 0, 0, 255);
		for(int x = space/2; x < width+2*r; x += space, i++) {
			if(i >= 0 && i < ptrbuflen)
				c = ptrbuf[i];
			else
				continue;
			if(c & 1) drawHole(x, 1*space, r);
			if(c & 2) drawHole(x, 2*space, r);
			if(c & 4) drawHole(x, 3*space, r);
			drawHole(x, 4*space, fr);
			if(c & 010) drawHole(x, 5*space,  r);
			if(c & 020) drawHole(x, 6*space,  r);
			if(c & 040) drawHole(x, 7*space,  r);
			if(c & 0100) drawHole(x, 8*space, r);
			if(c & 0200) drawHole(x, 9*space, r);
		}

		glDisable(GL_BLEND);
	}

	clearState();
}

void
drawPunch(Region *reg)
{
	setRegion(reg);

	int width = drawW;
	int height = drawH;

	int space = height/10;
	if(space < 4) space = 4;
	// heuristics
	int r = space/2 - 1;
	int fr = r/2 + 1;
	if(fr == r) fr--;

	int i, c;

	int punchpos = width-5*space;


	setColor(80, 80, 80, 255);
	drawRectangle(0.0f, 0.0f, width, height);

	if(ptpfd >= 0) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		setColor(255, 255, 176, 255);
		for(i = 0; i < ptpbuflen; i++)
			if(ptpbuf[i] < 0)
				break;
		int x = punchpos - i*space;
		if(x < 0) x = 0;
		drawRectangle(x, 0.0f, width, height);

		setColor(0, 0, 0, 255);
		i = 0;
		for(int x = punchpos; x > -2*r; x -= space) {
			if(i < ptpbuflen)
				c = ptpbuf[i++];
			else
				c = -1;
			if(c < 0)
				continue;
			if(c & 1) drawHole(x, 1*space, r);
			if(c & 2) drawHole(x, 2*space, r);
			if(c & 4) drawHole(x, 3*space, r);
			drawHole(x, 4*space, fr);
			if(c & 010) drawHole(x, 5*space,  r);
			if(c & 020) drawHole(x, 6*space,  r);
			if(c & 040) drawHole(x, 7*space,  r);
			if(c & 0100) drawHole(x, 8*space, r);
			if(c & 0200) drawHole(x, 9*space, r);
		}

		glDisable(GL_BLEND);
	}

	clearState();
}

// don't call when still mounted!
void
mountptr(const char *file)
{
	FILE *f = fopen(file, "rb");
	if(f == nil) {
		fprintf(stderr, "couldn't open file %s\n", file);
		return;
	}
	fseek(f, 0, 2);
	ptrbuflen = ftell(f);
	fseek(f, 0, 0);
	ptrbuf = malloc(ptrbuflen);
	fread(ptrbuf, 1, ptrbuflen, f);
	fclose(f);

	ptrpos = 0;
	// find beginning
	while(ptrpos < ptrbuflen && ptrbuf[ptrpos] == 0)
		ptrpos++;
	if(ptrpos > 20) ptrpos -= 10;
}

void
unmountptr(void)
{
	// i'm lazy...try to mitigate race somewhat
	static char unsigned dummy[100], *p;
	ptrbuflen = 0;
	p = ptrbuf;
	ptrbuf = dummy;
	if(p != dummy)
		free(p);
}

void
ptrsend(int fd)
{
	char c;
	c = ptrbuf[ptrpos];
	write(fd, &c, 1);
}

void
disconnectptr(struct pollfd *pfd)
{
	close(pfd->fd);
	pfd->fd = -1;
	pfd->events = 0;
}

void
connectptr(struct pollfd *pfd)
{
	pfd->fd = dial(host, 1042);
	if(pfd->fd < 0) {
		pfd->events = 0;
		unmountptr();
	} else {
		pfd->events = POLLIN;
		ptrsend(pfd->fd);
	}
}




void
initptp(void)
{
	ptpfd = open("/tmp/ptpunch", O_CREAT|O_WRONLY|O_TRUNC, 0644);
	if(ptpfd < 0) {
		fprintf(stderr, "can't open /tmp/ptpunch\n");
		exit(1);
	}
	for(int i = 0; i < ptpbuflen; i++)
		ptpbuf[i] = -1;
}

void
fileptp(const char *file)
{
	int fd;
	char c;

	fd = open(file, O_CREAT|O_WRONLY|O_TRUNC, 0644);
	if(fd < 0) {
		fprintf(stderr, "couldn't open file %s\n", file);
		return;
	}
	close(ptpfd);
	ptpfd = open("/tmp/ptpunch", O_RDONLY);
	while(read(ptpfd, &c, 1) > 0)
		write(fd, &c, 1);
	close(fd);
	close(ptpfd);
	initptp();
}

void
disconnectptp(struct pollfd *pfd)
{
	close(pfd->fd);
	pfd->fd = -1;
	pfd->events = 0;
}

void
connectptp(struct pollfd *pfd)
{
	pfd->fd = dial(host, 1043);
	if(pfd->fd < 0) {
		pfd->events = 0;
		initptp();
	} else {
		pfd->events = POLLIN;
	}
}

struct pollfd pfds[5];

void
cli(char *line, int n)
{
	char *p;

	if(p = strchr(line, '\r'), p) *p = '\0';
	if(p = strchr(line, '\n'), p) *p = '\0';

	char **args = split(line, &n);
	if(n > 0) {
		if(strcmp(args[0], "r") == 0) {
			disconnectptr(&pfds[1]);
			unmountptr();
			if(args[1]) {
				mountptr(args[1]);
				if(ptrbuflen > 0)
					connectptr(&pfds[1]);
			}
		}
		else if(strcmp(args[0], "p") == 0) {
			disconnectptp(&pfds[2]);
			if(args[1])
				fileptp(args[1]);
			else
				initptp();
			connectptp(&pfds[2]);
		}
	}
}

void*
tapethread(void *arg)
{
	char c;
	memset(pfds, 0, sizeof(pfds));

	pfds[0].fd = clifd[0];
	pfds[0].events = POLLIN;
	pfds[1].fd = -1;
	pfds[2].fd = -1;
	pfds[3].fd = socketlisten(1050);
	pfds[3].events = POLLIN;
	pfds[4].fd = -1;


	if(ptrbuflen > 0)
		connectptr(&pfds[1]);
	connectptp(&pfds[2]);

	int n;
	char line[1024];
	for(;;) {
		int ret = poll(pfds, nelem(pfds), -1);
		if(ret < 0)
			exit(0);
		if(ret == 0)
			continue;

		// accept cli connection
		if(pfds[3].revents & POLLIN) {
			struct sockaddr_in client;
			socklen_t len = sizeof(client);
			pfds[4].fd = accept(pfds[3].fd, (struct sockaddr*)&client, &len);
			if(pfds[4].fd >= 0)
				pfds[4].events = POLLIN;
		}

		// handle network cli
		else if(pfds[4].revents & POLLIN) {
                        n = read(pfds[4].fd, line, sizeof(line));
			if(n <= 0) {
				close(pfds[4].fd);
				pfds[4].fd = -1;
				pfds[4].events = 0;
			}
			if(n < sizeof(line))
				line[n] = '\0';
			cli(line, n);
			tapeUpdated = 1;
		}

		// handle cli
		else if(pfds[0].revents & POLLIN) {
			n = read(pfds[0].fd, line, sizeof(line));
			if(n <= 0) {
				close(pfds[0].fd);
				pfds[0].fd = -1;
				pfds[0].events = 0;
				continue;
			}
			if(n < sizeof(line))
				line[n] = '\0';
			cli(line, n);
			tapeUpdated = 1;
		}

		// handle reader
		else if(pfds[1].revents & POLLIN) {
			if(read(pfds[1].fd, &c, 1) <= 0) {
				disconnectptr(&pfds[1]);
				unmountptr();
			} else {
				ptrpos++;
				ptrsend(pfds[1].fd);
			}
			tapeUpdated = 1;
		}

		// handle punch
		else if(pfds[2].revents & POLLIN) {
			if(read(pfds[2].fd, &c, 1) <= 0) {
				disconnectptp(&pfds[2]);
				initptp();
				connectptp(&pfds[2]);
				continue;
			}
			memcpy(ptpbuf+1, ptpbuf, (ptpbuflen-1)*sizeof(*ptpbuf));
			ptpbuf[0] = c&0377;
			write(ptpfd, &c, 1);
			tapeUpdated = 1;
		}
	}
}

int
doDrawTape(void)
{
	int d = tapeUpdated;
	tapeUpdated = 0;
	return d;
}

#endif
