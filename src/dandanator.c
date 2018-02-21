/*
    ZEsarUX  ZX Second-Emulator And Released for UniX
    Copyright (C) 2013 Cesar Hernandez Bano

    This file is part of ZEsarUX.

    ZEsarUX is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>

#if defined(__APPLE__)
        #include <sys/syslimits.h>
#endif


#include "dandanator.h"
#include "cpu.h"
#include "debug.h"
#include "utils.h"
#include "operaciones.h"
#include "ula.h"


z80_bit dandanator_enabled={0};

char dandanator_rom_file_name[PATH_MAX]="";

z80_byte *dandanator_memory_pointer;


//Rutinas originales antes de cambiarlas
//void (*dandanator_original_poke_byte)(z80_int dir,z80_byte valor);
//void (*dandanator_original_poke_byte_no_time)(z80_int dir,z80_byte valor);
//z80_byte (*dandanator_original_peek_byte)(z80_int dir);
//z80_byte (*dandanator_original_peek_byte_no_time)(z80_int dir);

//puntero a cpu core normal sin dandanator
//void (*cpu_core_loop_no_dandanator) (void);

z80_bit dandanator_switched_on={0};
//z80_bit dandanator_accepting_commands={0};

//z80_bit dandanator_status_blocked={0};

int dandanator_nested_id_core;
int dandanator_nested_id_poke_byte;
int dandanator_nested_id_poke_byte_no_time;
int dandanator_nested_id_peek_byte;
int dandanator_nested_id_peek_byte_no_time;

/*
El Dandanator tiene tres formas de comportarse ante peticiones del Spectrum (las escrituras a rom):
  	A) Deshabilitado : Pasa de ellas.
	B) Bloqueado : Espera un comando especial específico (46) para pasar al estado C. Si llega cualquier otra cosa, lo ignora.
	C) Activo: Atiende a cualquier comando.
*/

//Estado A: dandanator_switched_on.v=0
//Estado B: dandanator_switched_on.v=1. dandanator_status_blocked.v=1
//Estado C: dandanator_switched_on.v=1

//Banco activo. 0..31 dandanator. 32=rom interna de spectrum
z80_byte dandanator_active_bank=0;


z80_byte dandanator_received_command;
z80_byte dandanator_received_data1;
z80_byte dandanator_received_data2;

z80_byte dandanator_contador_command; //Veces recibido
//z80_byte dandanator_contador_data1; //Veces recibido
//z80_byte dandanator_contador_data2; //Veces recibido


//0=Standby. Acabado de inicializar o finalizado un comando
//1=Recibidiendo comando
//#define DANDANATOR_COMMAND_STATUS_STANDBY 0
//#define DANDANATOR_COMMAND_STATUS_RECEIVING_COMMAND 1
//#define Pending_Executing_Command 2
//int dandanator_command_state;

//t_stados que hay que esperar antes de ejecutar comando
unsigned int dandanator_needed_t_states_command;

typedef enum                                                                    // State Machine for incoming communications from Spectrum
{
  Commands_Disabled,
  Commands_Locked,
  Wait_Normal,
  In_Progress_Normal,
  In_Progress_Locked,
  In_Progress_Special_Data1,
  In_Progress_Special_Data2,
  In_Progress_Fast,
  Pending_Processing,
  Pending_Executing_Command  //Este estado me dice que tengo que esperar determinados t-estados antes de ejecutar
} ZXCmdStateMachine;


/*
Esos son los estados, te los voy contando.

Commands_Disabled es el estado en el que arranca el Dandanator. Sólo cambia a Wait_Normal (que es que acepta comandos) si se selecciona el Slot 0 (o alguno de los dos slots definidos por el comando 49, que creo que no lo tienes implementado, pero no es importante ahora)
Se selecciona el slot 0 siempre al darle al botón. A este estado se vuelve mediante un comando 46 o mediante un comando 40 con el bit 3 activo. Date cuenta que el dandanator no cambia de Slot ni de rom externa/interna por ninguno de sus estados. Eso hay que cambiarlo explícitamente.

Commands_Locked es el estado al que se accede por el comando 46 o el comando 40 con el bit 2 activo. Ese lo tienes controlado. Sólo se sale de él volviendo al slot 0 (botón) o con un comando 46.

In_progress_Normal Se están recibiendo pulsos de un COMANDO, en tu caso es la parte en la que recibes cosas en la dirección 0x0001

In_progress_Locked Se están recibiendo pulsos de un COMANDO 46. (lo mismo que antes, pero veníamos de estar locked)

In_progress_Special_Data1/ In_progress_Special_Data2 se están recibiendo datos, en tu caso es cuando recibes cosas en las direcciones 0x0002 y 0x0003 respectivamente. Estos estados son iguales independientemente de que partiéramos de Wait_Normal o Commands_Locked.

In_progress_fast Se han recibido Comando, data1 y data 2 y se está esperando a recibir el pulso de confirmación (en tu caso, el pulso en la dirección 0x0000)

Pending processing, los comandos normales, los de 1 byte, se procesan el bucle principal. Este estado indica que hay comandos pendientes de procesar. Cuando se procesan se vuelve a Wait_Normal o a Commands_Disabled en el caso del comando 34.


Resumen importante. Ningún comando que no cambie explícitamente el banco del dandanator, debe producir cambios sobre los bancos, ya sean externos o internos. El botón equivale a un comando 40,0,1. Los comandos se habilitan sólos (Wait_Normal) por estar en el slot 0.

*/



ZXCmdStateMachine dandanator_state;


void dandanator_run_command_46(void)
{
                        if (dandanator_received_data1!=dandanator_received_data2) {
                                debug_printf (VERBOSE_DEBUG,"Dandanator: Data1 != Data2. Ignoring");
                                return;
                        }

	if (dandanator_received_data1==1) dandanator_state=Commands_Locked;
	if (dandanator_received_data1==16) dandanator_state=Wait_Normal;
	if (dandanator_received_data1==31) dandanator_state=Commands_Disabled;
}


void dandanator_run_command(void)
{
	debug_printf (VERBOSE_DEBUG,"Dandanator: Running command %d",dandanator_received_command);
	if (dandanator_state==Commands_Locked) {
		if (dandanator_received_command==46) {
			/*
Comando especial 46: Bloquear, desbloquear, deshabilitar. – Este comando bloquea, desbloquea o deshabilita futuros comandos. Es el único comando reconocido si los comandos están bloqueados.
Parámetros:
- Data 1: 1 para bloquear, 16 para desbloquear y habilitar comandos, 31 para deshabilitar comandos hasta un reboot frío. - Data 2: Debe ser igual a Data1 o el comando será ignorado.
			*/
			debug_printf (VERBOSE_DEBUG,"Dandanator: was in blocked mode. Received command 46 with data1 %d data2 %d",
		                dandanator_received_data1,dandanator_received_data2);

			dandanator_run_command_46();


		}

		else {
			debug_printf (VERBOSE_DEBUG,"Dandanator: is in blocked mode and command received is not 46. Not accepting anything");
		}

		return;
	}

	if (dandanator_received_command>=1 && dandanator_received_command<=33) {
		//Cambio banco de rom
		dandanator_active_bank=dandanator_received_command-1;
		debug_printf (VERBOSE_DEBUG,"Dandanator: Switching to dandanator bank %d",dandanator_active_bank);
	}

	else if (dandanator_received_command==34) {
		//El comando 34 -> es como el 33 pero bloquea futuros comandos.
                dandanator_state=Commands_Locked;
                debug_printf (VERBOSE_DEBUG,"Dandanator: Switching to normal ROM and locking dandanator future commands until command 46");
		dandanator_active_bank=32;
	}

	else if (dandanator_received_command==36) {
		reset_cpu();
	}

	else if (dandanator_received_command==39) {
		//TODO: establece el slot actual como el slot al que hay que volver si se pulsa un reset—>
		//Esto es, si recibes un comando 39 y estas en el slot 14 por ejemplo, cuando se pulse un reset en el
		// spectrum, independientemente de en qué slot estés, se selecciona primero el slot 14 y se hace un reset.
	}



	else if (dandanator_received_command>=40 && dandanator_received_command<=49) {
		debug_printf (VERBOSE_DEBUG,"Dandanator: Running special command %d with data1 %d data2 %d",
		dandanator_received_command,dandanator_received_data1,dandanator_received_data2);

		switch (dandanator_received_command) {
			case 40:
/*
Comando especial 40: Cambio Rápido – Este comando cambia a un banco determinado y ejecuta una acción en el momento de recibir el pulso de confirmación. Normalmente se usa para devolver el control a un software cambiando rápidamente de banco sin esperar los 5ms de la ventana de cambio normal. Pruebas empíricas determinan el cambio de banco en el rango de los 12us.
Parámetros:
- Data 1: Número de banco para ejecutar el cambio.
- Data 2: Acción a ejecutar tras el cambio de banco (mascara de bits)
o Bits 4-7: Reservados. Siempre 0.
o Bit 3: Desactivar comandos futuros. Desactiva la recepción de comandos futuros por parte del Dandanator hasta un reset frio.
o Bit 2: Bloquear comandos futuros. Desactiva la recepción de comandos futuros hasta un comando especial determinado (46).
o Bit 1: NMI. Ejecuta una NMI tras cambiar el banco.
o Bit 0: Reset. Ejecuta un Reset del Z80 tras el cambio de banco.
*/

				dandanator_active_bank=dandanator_received_data1-1;
				debug_printf (VERBOSE_DEBUG,"Dandanator: Switching to dandanator bank %d and run some actions: %d",dandanator_active_bank,dandanator_received_data2);
				if (dandanator_received_data2 & 8) {
					debug_printf (VERBOSE_DEBUG,"Dandanator: Disabling dandanator");
					dandanator_state=Commands_Disabled;
				}
				if (dandanator_received_data2 & 4) {
					dandanator_state=Commands_Locked;
					debug_printf (VERBOSE_DEBUG,"Dandanator: Locking dandanator future commands until command 46");
				}

				if (dandanator_received_data2 & 2) {
					generate_nmi();
				}

				if (dandanator_received_data2 & 1) {
					reset_cpu();
				}

			break;

			case 41:
			case 42:
			case 43:
				debug_printf (VERBOSE_DEBUG,"Dandanator: Unusable on emulation");

			break;

			case 46:
				 dandanator_run_command_46();
			break;

			default:
				debug_printf (VERBOSE_DEBUG,"Dandanator: UNKNOWN command %d",dandanator_received_command);
			break;
		}
	}

	else {
		debug_printf (VERBOSE_DEBUG,"Dandanator: UNKNOWN command %d",dandanator_received_command);
	}
}

void dandanator_set_pending_run_command(void)
{
	dandanator_state=Pending_Executing_Command;
	debug_t_estados_parcial=0;
	debug_printf (VERBOSE_DEBUG,"Dandanator: Schedule pending command %d after %d t-states. PC=%d",dandanator_received_command,dandanator_needed_t_states_command,reg_pc);
}

void dandanator_write_byte(z80_int dir,z80_byte valor)
{
	if (dandanator_state!=Commands_Disabled) {
		//printf ("Escribiendo dir %d valor %d PC=%d\n",dir,valor,reg_pc);

		if (dir==0) {
			//Confirmacion de un comando de 3 bytes.

			//Ver si estabamos recibiendo comando
			if (dandanator_state==In_Progress_Fast) return;

			//Contador para ejecutar comando especial despues de 35 t-estados
			dandanator_needed_t_states_command=35;
			dandanator_set_pending_run_command();

		}

		if (dir==1) {
			if (dandanator_state==Wait_Normal) {
				dandanator_contador_command=0;
				dandanator_received_command=valor;
				dandanator_state=In_Progress_Normal;
			}

			if (dandanator_state==In_Progress_Normal) {
				dandanator_contador_command++;

				//Si comando de 1 byte y se ha recibido el byte tantas veces como el valor de comando, ejecutar
				if (valor<40 && valor==dandanator_contador_command) {
					//Contador para ejecutar comando simple despues de X t-estados
					if (MACHINE_IS_SPECTRUM_16_48) {
						dandanator_needed_t_states_command=119;
					}
					else {
						dandanator_needed_t_states_command=121;
					}

					dandanator_set_pending_run_command();

				}
				else {
					//Si valor recibido no era el que habia de comando, resetear. Esto no deberia pasar...
					if (valor!=dandanator_received_command) {
						dandanator_state=In_Progress_Normal;
						dandanator_contador_command=1;
						dandanator_received_command=valor;
						debug_printf (VERBOSE_DEBUG,"Dandanator: Received different command before finishing previous...");
					}
				}


			}
		}

		if (dir==2) {
			dandanator_received_data1=valor;  //Para simplificar no contamos numero de veces
		}

		if (dir==3) {
			dandanator_received_data2=valor;  //Para simplificar no contamos numero de veces
		}


	}
}


int dandanator_check_if_rom_area(z80_int dir)
{
        if (dandanator_switched_on.v) {
                if (dir<16384) {
			return 1;
                }
        }
	return 0;
}

z80_byte dandanator_read_byte(z80_int dir)
{
	//Si banco apunta a 32, es la rom interna
	//if (dandanator_active_bank==32) return dandanator_original_peek_byte_no_time(dir);
	if (dandanator_active_bank==32) return debug_nested_peek_byte_no_time_call_previous(dandanator_nested_id_peek_byte_no_time,dir);

	//Si no, memoria dandanator
	int puntero=dandanator_active_bank*16384+dir;
	return dandanator_memory_pointer[puntero];
}


z80_byte dandanator_poke_byte(z80_int dir,z80_byte valor)
{

	//dandanator_original_poke_byte(dir,valor);
        //Llamar a anterior
        debug_nested_poke_byte_call_previous(dandanator_nested_id_poke_byte,dir,valor);

	if (dandanator_check_if_rom_area(dir)) {
		dandanator_write_byte(dir,valor);
	}

	//temp
	//if (dir<16384 && !dandanator_check_if_rom_area(dir)) {
	//	printf ("Recibido write en %d valor %d con dandanator desactivado\n",dir,valor);
	//}

        //Para que no se queje el compilador, aunque este valor de retorno no lo usamos
        return 0;


}

z80_byte dandanator_poke_byte_no_time(z80_int dir,z80_byte valor)
{
        //dandanator_original_poke_byte_no_time(dir,valor);
        //Llamar a anterior
        debug_nested_poke_byte_no_time_call_previous(dandanator_nested_id_poke_byte_no_time,dir,valor);


	if (dandanator_check_if_rom_area(dir)) {
                dandanator_write_byte(dir,valor);
        }

	//temp
	//if (dir<16384 && !dandanator_check_if_rom_area(dir)) {
	//	printf ("Recibido write en %d valor %d con dandanator desactivado\n",dir,valor);
	//}

        //Para que no se queje el compilador, aunque este valor de retorno no lo usamos
        return 0;


}

z80_byte dandanator_peek_byte(z80_int dir,z80_byte value GCC_UNUSED)
{

	z80_byte valor_leido=debug_nested_peek_byte_call_previous(dandanator_nested_id_peek_byte,dir);

	if (dandanator_check_if_rom_area(dir)) {
       		//t_estados +=3;
		return dandanator_read_byte(dir);
	}

	//return dandanator_original_peek_byte(dir);
	return valor_leido;
}

z80_byte dandanator_peek_byte_no_time(z80_int dir,z80_byte value GCC_UNUSED)
{

	z80_byte valor_leido=debug_nested_peek_byte_no_time_call_previous(dandanator_nested_id_peek_byte_no_time,dir);

	if (dandanator_check_if_rom_area(dir)) {
                return dandanator_read_byte(dir);
        }

	//else return debug_nested_peek_byte_no_time_call_previous(dandanator_nested_id_peek_byte_no_time,dir);
	return valor_leido;
}



//Establecer rutinas propias
void dandanator_set_peek_poke_functions(void)
{
                debug_printf (VERBOSE_DEBUG,"Setting dandanator poke / peek functions");
                //Guardar anteriores
                //dandanator_original_poke_byte=poke_byte;
                //dandanator_original_poke_byte_no_time=poke_byte_no_time;
                //dandanator_original_peek_byte=peek_byte;
                //dandanator_original_peek_byte_no_time=peek_byte_no_time;

                //Modificar y poner las de dandanator
                //poke_byte=dandanator_poke_byte;
                //poke_byte_no_time=dandanator_poke_byte_no_time;
                //peek_byte=dandanator_peek_byte;
                //peek_byte_no_time=dandanator_peek_byte_no_time;


	//Asignar mediante nuevas funciones de core anidados
	dandanator_nested_id_poke_byte=debug_nested_poke_byte_add(dandanator_poke_byte,"Dandanator poke_byte");
	dandanator_nested_id_poke_byte_no_time=debug_nested_poke_byte_no_time_add(dandanator_poke_byte_no_time,"Dandanator poke_byte_no_time");
	dandanator_nested_id_peek_byte=debug_nested_peek_byte_add(dandanator_peek_byte,"Dandanator peek_byte");
	dandanator_nested_id_peek_byte_no_time=debug_nested_peek_byte_no_time_add(dandanator_peek_byte_no_time,"Dandanator peek_byte_no_time");

}

//Restaurar rutinas de dandanator
void dandanator_restore_peek_poke_functions(void)
{
                debug_printf (VERBOSE_DEBUG,"Restoring original poke / peek functions before dandanator");
                //poke_byte=dandanator_original_poke_byte;
                //poke_byte_no_time=dandanator_original_poke_byte_no_time;
                //peek_byte=dandanator_original_peek_byte;
                //peek_byte_no_time=dandanator_original_peek_byte_no_time;


	debug_nested_poke_byte_del(dandanator_nested_id_poke_byte);
	debug_nested_poke_byte_no_time_del(dandanator_nested_id_poke_byte_no_time);
	debug_nested_peek_byte_del(dandanator_nested_id_peek_byte);
	debug_nested_peek_byte_no_time_del(dandanator_nested_id_peek_byte_no_time);
}

/*
void old_cpu_core_loop_dandanator(void)
{

     cpu_core_loop_no_dandanator();


        if (dandanator_state==Pending_Executing_Command) {
                if (debug_t_estados_parcial>dandanator_needed_t_states_command) {
                        debug_printf (VERBOSE_DEBUG,"Dandanator: Run command after needed %d t-states",dandanator_needed_t_states_command);
                        dandanator_run_command();
                        //Volver a standby normalmente.
			if (dandanator_state==Pending_Executing_Command) dandanator_state=Wait_Normal;
                }
        }


}
*/


z80_byte cpu_core_loop_dandanator(z80_int dir GCC_UNUSED, z80_byte value GCC_UNUSED)
{
	//Llamar a anterior
	debug_nested_core_call_previous(dandanator_nested_id_core);

        if (dandanator_state==Pending_Executing_Command) {
                if (debug_t_estados_parcial>dandanator_needed_t_states_command) {
                        debug_printf (VERBOSE_DEBUG,"Dandanator: Run command after needed %d t-states",dandanator_needed_t_states_command);
                        dandanator_run_command();
                        //Volver a standby normalmente.
                        if (dandanator_state==Pending_Executing_Command) dandanator_state=Wait_Normal;
                }
        }

	//Para que no se queje el compilador, aunque este valor de retorno no lo usamos
	return 0;

}

/*
void old_dandanator_set_core_function(void)
{
                debug_printf (VERBOSE_DEBUG,"Setting dandanator Core loop");
                //Guardar anterior
                cpu_core_loop_no_dandanator=cpu_core_loop;

                //Modificar
                cpu_core_loop=cpu_core_loop_dandanator;
}
*/


void dandanator_set_core_function(void)
{
	debug_printf (VERBOSE_DEBUG,"Setting dandanator Core loop");
	//Asignar mediante nuevas funciones de core anidados
	dandanator_nested_id_core=debug_nested_core_add(cpu_core_loop_dandanator,"Dandanator core");
}


/*
void old_dandanator_restore_core_function(void)
{
        debug_printf (VERBOSE_DEBUG,"Restoring original dandanator core");
        cpu_core_loop=cpu_core_loop_no_dandanator;
}
*/

void dandanator_restore_core_function(void)
{
        debug_printf (VERBOSE_DEBUG,"Restoring original dandanator core");
	debug_nested_core_del(dandanator_nested_id_core);
}



void dandanator_alloc_memory(void)
{
        int size=(DANDANATOR_SIZE+256);  //+256 bytes adicionales para la memoria no volatil

        debug_printf (VERBOSE_DEBUG,"Allocating %d kb of memory for dandanator emulation",size/1024);

        dandanator_memory_pointer=malloc(size);
        if (dandanator_memory_pointer==NULL) {
                cpu_panic ("No enough memory for dandanator emulation");
        }


}

int dandanator_load_rom(void)
{

        FILE *ptr_dandanator_romfile;
        int leidos=0;

        debug_printf (VERBOSE_INFO,"Loading dandanator rom %s",dandanator_rom_file_name);

  			ptr_dandanator_romfile=fopen(dandanator_rom_file_name,"rb");
                if (!ptr_dandanator_romfile) {
                        debug_printf (VERBOSE_ERR,"Unable to open ROM file");
                }

        if (ptr_dandanator_romfile!=NULL) {

                leidos=fread(dandanator_memory_pointer,1,DANDANATOR_SIZE,ptr_dandanator_romfile);
                fclose(ptr_dandanator_romfile);

        }



        if (leidos!=DANDANATOR_SIZE || ptr_dandanator_romfile==NULL) {
                debug_printf (VERBOSE_ERR,"Error reading dandanator rom");
                return 1;
        }

        return 0;
}



void dandanator_enable(void)
{

  if (!MACHINE_IS_SPECTRUM) {
    debug_printf(VERBOSE_INFO,"Can not enable dandanator on non Spectrum machine");
    return;
  }

	if (dandanator_enabled.v) return;

	if (dandanator_rom_file_name[0]==0) {
		debug_printf (VERBOSE_ERR,"Trying to enable Dandanator but no ROM file selected");
		return;
	}

	dandanator_alloc_memory();
	if (dandanator_load_rom()) return;

	dandanator_set_peek_poke_functions();

	dandanator_set_core_function();


	dandanator_switched_on.v=0;
	//dandanator_accepting_commands.v=0;
	dandanator_state=Commands_Disabled;
	dandanator_active_bank=0;

	dandanator_enabled.v=1;


}

void dandanator_disable(void)
{
	if (dandanator_enabled.v==0) return;

	dandanator_restore_peek_poke_functions();

	free(dandanator_memory_pointer);

	dandanator_restore_core_function();

	dandanator_enabled.v=0;
}


void dandanator_press_button(void)
{

        if (dandanator_enabled.v==0) {
                debug_printf (VERBOSE_ERR,"Trying to press Dandanator button when it is disabled");
                return;
        }

        dandanator_switched_on.v=1;
	//dandanator_accepting_commands.v=1;
	dandanator_active_bank=0;
	dandanator_state=Wait_Normal;
	//dandanator_status_blocked.v=0;

	reset_cpu();


}
