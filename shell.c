#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <glob.h>

enum {
	MAXLINE = 256,
	MAXARGS = 64,
	MAXPATH = 128,
	MAXENVS = 16,
	MAXINT = 10,
	// MAXCMDS = 64,
};

struct Env {
	char *name;
	char *value;
};
typedef struct Env Env;

struct Envs {
	Env **arr;
	int elems;
};
typedef struct Envs Envs;

struct Comm {
	/* 
	int n_commands;
	char *commands[MAXCMDS][MAXARGS]; 
	int pids[MAXCMDS - 1];
	*/
	char *command;
	char *args[MAXARGS];
	int n_args;
	char *in_file;
	char *out_file;
	int bg;
	int here;
	int sts;		// Estado de salida
};
typedef struct Comm Comm;

void
delete_eol(char *line)
{
	if (line[strlen(line) - 1] == '\n')
		line[strlen(line) - 1] = '\0';
}

char *
int_to_str(int i, char *str)
{
	snprintf(str, MAXINT, "%d", i);
	return str;
}

void
shift_args(char **args, int n_args)	// Desplazo una posicion lo argumentos, para ignorar el primero "ifok/ifnot"
{
	for (int i = 1; i < n_args; i++) {
		args[i - 1] = args[i];
	}
	args[n_args - 1] = NULL;
	n_args--;
}

void
redirect_input(char *in_file)
{
	int fd = open(in_file, O_RDONLY);

	if (fd < 0) {
		perror("Error: abriendo fichero-entrada");
		exit(EXIT_FAILURE);
	}
	dup2(fd, STDIN_FILENO);
	close(fd);
}

void
redirect_output(char *out_file)
{
	int fd = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (fd < 0) {
		perror("Error: abriendo fichero-salida");
		exit(EXIT_FAILURE);
	}
	dup2(fd, STDOUT_FILENO);
	close(fd);
}

void
handle_redirects(char *in_file, char *out_file)
{
	if (in_file)
		redirect_input(in_file);
	if (out_file)
		redirect_output(out_file);
}

void
handle_bg(int bg)
{
	if (bg)
		redirect_input("/dev/null");
}

void
handle_here(Comm *c)
{
	char line[MAXLINE];
	int pipe_fd[2];

	if (pipe(pipe_fd) == -1) {
		perror("Error: pipe_fd");
		exit(EXIT_FAILURE);
	}
	while (fgets(line, MAXLINE, stdin)) {
		delete_eol(line);	// Elimina el salto de línea
		if (strcmp(line, "}") == 0)
			break;	// Final del documento "here"
		write(pipe_fd[1], line, strlen(line));
		write(pipe_fd[1], "\n", 1);	// Asegura un salto de línea al final
	}
	close(pipe_fd[1]);	// Cierra el extremo de escritura

	// Redirigir la entrada estándar al extremo de lectura de la tubería
	dup2(pipe_fd[0], STDIN_FILENO);
	close(pipe_fd[0]);	// Cierra el extremo de lectura
}

int
find_comm(char *command, char *path)
{
	char *path_env = getenv("PATH");

	if (path_env == NULL) {
		perror("Error: PATH");
		exit(EXIT_FAILURE);
	}
	char *dir = strtok_r(path_env, ":", &path_env);	// Separa los directorios por ":"

	while (dir != NULL) {
		snprintf(path, MAXPATH, "%s/%s", dir, command);
		if (access(path, X_OK) == 0)
			return 0;
		dir = strtok_r(NULL, ":", &path_env);	// Pasa al siguiente directorio
	}
	return 1;		// No se encontró el comando
}

int
is_asign(char *line)
{
	return strstr(line, "=") != NULL;
}

void
set_new_env(Envs *vs, char *line)
{
	if (vs->elems > MAXENVS)
		return;
	Env *v = malloc(sizeof(Env));
	char *name = strtok_r(line, "=", &line);

	if (name && strcmp(name, vs->arr[0]->name)) {	// Si es diferente de "result"
		v->name = strdup(name);
		char *value = strtok_r(NULL, " ", &line);

		if (value) {
			v->value = strdup(value);
			vs->arr[vs->elems++] = v;
			return;
		}
		free(v->name);
		free(v);
	}
	free(v);
}

void
set_command(Comm *c, char *line)
{
	memset(c, 0, sizeof(Comm));

	char *token = strtok_r(line, " ", &line);

	while (token) {
		if (strcmp(token, "<") == 0) {
			c->in_file = strtok_r(NULL, " ", &line);
		} else if (strcmp(token, ">") == 0) {
			c->out_file = strtok_r(NULL, " ", &line);
		} else if (strcmp(token, "&") == 0) {
			c->bg = 1;
		} else if (strcmp(token, "HERE{") == 0) {	// OPCIONAL 1
			c->here = 1;
		} else {
			c->args[c->n_args++] = token;
		}
		token = strtok_r(NULL, " ", &line);
	}
	c->args[c->n_args] = NULL;
}

char *
get_env_value(Envs *vs, const char *name)
{
	for (int i = 0; i < vs->elems; i++) {
		if (strcmp(vs->arr[i]->name, name) == 0)
			return vs->arr[i]->value;
	}
	return NULL;
}

int
replace_envs(Envs *vs, Comm *c)
{
	for (int i = 0; i < c->n_args; i++) {	// Recorremos los argumentos de la línea de comandos
		char *arg = c->args[i];

		if (arg[0] == '$') {	// Si es una variable de entorno
			char *env_name = &arg[1];	// Ignoramos el primer '$'
			char *env_value = NULL;

			if (!strcmp(&arg[1], vs->arr[0]->name))	// Si es concretamente la variable de entorno: $result (c->sts.name)
				env_value = vs->arr[0]->value;
			else	// Si es cualquier otra variable de entorno
				env_value = get_env_value(vs, env_name);

			if (env_value)
				c->args[i] = strdup(env_value);	// Sustituimos la variable por su valor
			else {
				fprintf(stderr,
					"Error: Variable %s no existe\n",
					env_name);
				return 0;	// Salimos si no se encuentra la variable
			}
		}
	}
	return 1;
}

void
exec_nobuiltin(Comm *c)
{
	char path[MAXPATH];
	int pidwait;
	int sts;
	int pidchild = fork();

	if (pidchild < 0)
		perror("Error: fork");
	else if (pidchild == 0) {	// Proceso hijo (comando)
		if (c->here)
			handle_here(c);
		else {
			handle_redirects(c->in_file, c->out_file);
			handle_bg(c->bg);
		}
		if (find_comm(c->command, path) == 0) {
			execv(path, c->args);
			perror("Error: execv");
		} else
			fprintf(stderr, "No se ha encontrado la orden «%s»\n",
				c->command);
		exit(EXIT_FAILURE);
	} else {		// Proceso padre (shell)
		if (!c->bg) {
			do {
				pidwait = wait(&sts);
				if (WIFEXITED(sts))
					c->sts = WEXITSTATUS(sts);
			}
			while (pidwait != -1 && pidwait != pidchild);
			if (pidwait != pidchild)
				warnx("Se ha encontrado un hijo adicional %d",
				      pidwait);
		}
	}
}

void
handle_cd(Comm *c)
{
	if (c->n_args == 1) {
		char *home = getenv("HOME");

		if (home)
			c->sts = chdir(home);
		else
			c->sts = 1;
	} else {
		if (chdir(c->args[1]) != 0) {
			fprintf(stderr,
				"cd: No existe el archivo o el directorio\n");
		}
		c->sts = chdir(c->args[1]);
	}
}

void
handle_ifok(Envs *vs, Comm *c)
{
	if (strcmp(vs->arr[0]->value, "0") == 0) {
		shift_args(c->args, c->n_args);
		exec_nobuiltin(c);
	} else
		c->sts = 1;
}

void
handle_ifnot(Envs *vs, Comm *c)
{
	if (strcmp(vs->arr[0]->value, "0") != 0) {
		shift_args(c->args, c->n_args);
		exec_nobuiltin(c);
	} else
		c->sts = 1;
}

void
exec(Envs *vs, Comm *c)
{
	c->command = c->args[0];
	if (!strcmp(c->command, "cd"))
		handle_cd(c);
	else if (!strcmp(c->command, "ifok"))
		handle_ifok(vs, c);
	else if (!strcmp(c->command, "ifnot"))
		handle_ifnot(vs, c);
	else if (!strcmp(c->command, "exit"))
		exit(atoi(c->args[1]));
	else
		exec_nobuiltin(c);
}

void
set_env_result(Envs *vs, Comm *c)
{
	char sts_str[MAXINT];

	if (vs->arr[0]->value)
		free(vs->arr[0]->value);
	int_to_str(c->sts, sts_str);
	vs->arr[0]->value = strdup(sts_str);
}

void
free_envs(Envs *vs)
{
	for (int i = 0; i < vs->elems; i++) {
		free(vs->arr[i]->name);
		free(vs->arr[i]->value);
		free(vs->arr[i]);
	}
	free(vs->arr);
	free(vs);
}

void
expand_glob(Comm *c)
{
}

void
handle_line()
{
	if (feof(stdin)) {
		clearerr(stdin);
		printf("\n");
	} else
		perror("Error: leyendo entrada");
}

int
main(int argc, char *argv[])
{
	Comm c;
	Envs *vs;
	char line[MAXLINE];

	if (argc != 1)
		exit(EXIT_FAILURE);

	vs = (Envs *)malloc(sizeof(Envs));
	vs->elems = 1;		// Empieza con un elemento, porque el primero se reserva para "$result"
	vs->arr = (Env **)malloc(sizeof(Env *) * MAXENVS);
	vs->arr[0] = malloc(sizeof(Env));
	if (!vs || !vs->arr || !vs->arr[0]) {
		free_envs(vs);
		err(EXIT_FAILURE, "memoria desbordada");
	}
	vs->arr[0]->name = strdup("result");
	vs->arr[0]->value = strdup("0");

	while (1) {
		if (isatty(STDIN_FILENO))
			printf("$> ");
		if (fgets(line, MAXLINE, stdin) == NULL) {
			handle_line();
			break;
		}
		delete_eol(line);
		if (strlen(line) == 0)
			continue;
		if (is_asign(line))	// Si la linea de comando es una asignacion de variable de entorno:
			set_new_env(vs, line);
		else {		// Si la linea de comando es una ejecucion de comandos:
			set_command(&c, line);
			if (!replace_envs(vs, &c))
				continue;	// Si no encuentra la variable (no existe), paso a la siguiente linea de comandos
			expand_glob(&c);
			exec(vs, &c);
			set_env_result(vs, &c);
		}
	}
	free_envs(vs);
	exit(EXIT_SUCCESS);
}
