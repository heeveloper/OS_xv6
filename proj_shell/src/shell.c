/* 
 * @class 	: Operating System
 * @file		:	shell.c
 * @brief		:	Implementation of shell
 * @author	:	Heejun Lee (zaqq1414@gmail.com)
 * @since		:	2018-03-24
*/

#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_STRING 1024
#define MAX_COMMAND 512
#define MAX_OPTION 4

int main(int argc, char *argv[]){
		char input_str[MAX_STRING]; 
		char *section = NULL; // Pointer to the each command
		char *command[MAX_COMMAND][MAX_OPTION]; // Pointer array which store each command and options
		char *option; //Pointer to a command and options in a section

		int num_commands;
		int num_option;
		int idx ; //index for loop

		// Batch Mode
		if (argc == 2) {
				FILE *fp = fopen(argv[1], "r");
				if (fp == NULL){
						fprintf(stderr, "Cannot open the file %s\n", argv[1]);
						exit(0);
				} else {
						while (fgets(input_str, sizeof(input_str), fp) != NULL) {

								// Delete the '\n'
								input_str[strlen(input_str)-1] = '\0';
								printf("%s\n",input_str);

								// Multiple commands
								if (strchr(input_str, ';') != NULL){
										num_commands = 0;

										// To prevent duplicates of strtok pointers in the repetition,
										// use the strtok_r
										char *saved_ptr;
										section = strtok_r(input_str, ";", &saved_ptr);

										do {

												if (!strncmp(section, "quit", 4)) {
														printf("quit!\n");
														exit(0);
												}

												int num_option = 0;

												option = strtok(section, " ");
												while (option != NULL){
														command[num_commands][num_option] = option;
														num_option++;
														option = strtok(NULL, " ");
												}
												num_commands++;
										} while (section = strtok_r(NULL, ";", &saved_ptr));
								}  
								// Single command.
								else {
										num_commands = 1;
										num_option = 0;
										input_str[strlen(input_str)] = '\0';

										if (!strncmp(input_str, "quit", 4)) {
												exit(0);
										}

										option = strtok(input_str, " ");
										while (option != NULL) {
												command[0][num_option] = option;
												option = strtok(NULL, " ");
												num_option++;
										}
								}

								// Create new process to execute given command.
								for (idx = 0; idx < num_commands; idx++) {
										switch (fork()) {
												case -1: {
																		 perror("fork");
																		 break;
																 }
												case 0: {
																		execvp(command[idx][0], command[idx]);
																		printf("command not found\n");
																		exit(0);
																}
												default: {
																		 break;
																 }
										}
								}
								// Wait for every child processes to exit
								while (wait(NULL) != -1) 
										continue;

								// Initialize pointer arrays
								for(idx = 0; idx < MAX_STRING; idx++) {
										input_str[idx] = '\0';
								}
								for(idx = 0; idx < MAX_COMMAND; idx++) {
										int option;
										for(option = 0; option < MAX_OPTION; option++) {
												command[idx][option] = NULL;
										}
								}
						}
				}
				fclose(fp);
		} 
		// Interactive Mode.
		else { 				
				while (1) {
						//Initialize pointer arrays.
						for(idx = 0; idx < MAX_STRING; idx++) {
								input_str[idx] = '\0';
						}
						for(idx = 0; idx < MAX_COMMAND; idx++) {
								int option;
								for(option = 0; option < MAX_OPTION; option++) {
										command[idx][option] = NULL;
								}
						}

						printf("prompt> ");

						fgets(input_str, sizeof(input_str), stdin);

						input_str[strlen(input_str) - 1] = '\0';
						fflush(stdin);

						if (feof(stdin)) {
								printf("Ctrl+D exit\n");
								exit(0);
						}
						if (!strncmp(input_str, "quit", 4)) {
								exit(0);
						}


						// Multiple commands.
						if (strchr(input_str, ';') != NULL) {
								num_commands = 0;

								char *saved_ptr;
								section = strtok_r(input_str, ";", &saved_ptr);

								while (section != NULL) {
										num_option = 0;

										if (!strncmp(section, "quit", 4)) {
												exit(0);
										}

										option = strtok(section, " ");
										while (option != NULL) {
												command[num_commands][num_option] = option;
												option = strtok(NULL, " ");
												num_option++;
										}
										section = strtok_r(NULL, ";", &saved_ptr);
										num_commands++;
								}
						}
						// Single command.
						else{
								num_commands = 1;

								option = strtok(input_str, " ");
								num_option = 0;
								while(option != NULL){
										command[0][num_option] = option;
										option = strtok(NULL, " ");
										num_option++;
								}
						}
						// Create new process to execute given command.
						for (idx = 0; idx < num_commands; idx++) {
								switch (fork()) {
										case -1: {
																 perror("fork");
																 break;
														 }
										case 0: {
																execvp(command[idx][0], command[idx]);
																printf("command not found\n");
																exit(0);
														}
										default: {
																 break;
														 }
								}
						}
						// Wait for every child processes to exit/
						while (wait(NULL) != -1)
								continue;
				}
		}
		return 0;
}

