/* Copyright (c) 2002-2003 krzYszcz and others.
 * For information on usage and redistribution, and for a DISCLAIMER OF ALL
 * WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

//matt barber updated cycle~ in 2016
/*derek kwan - 2016: updated attributes,
  de-'sic'-ified, also de-'loud'-ified and de-'vefl'-ified
  added cycle_free,cycle_buffer, cycle_makecostab,cycle_fetcharray
  */

#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "m_pd.h"

#define PDCYCYCLE_FREQ 	0
#define PDCYCYCLE_PHASE 0
#define PDCYCYCLE_OFFSET 0
#define PDCYCYCLE_TABSIZE  512
#define COS_TABSIZE  16384

typedef struct _cycle
{
    t_object   x_obj;

	t_float    x_freq;
	t_float	   x_init_phase;
    double     x_phase;
    double     x_conv;
    t_symbol  *x_name;
    int        x_offset;
    int        x_cycle_tabsize; //how far to loop
    int		   x_use_all;
    t_float   *x_table;
    double    *x_costable;
    t_float   *x_usertable;
    t_float    x_usertableini[PDCYCYCLE_TABSIZE + 1];
    int        x_usertable_tabsize; // actual table size -- loop size might be smaller

	t_inlet    *x_phaselet;
	t_outlet   *x_outlet;
} t_cycle;

static t_class *cycle_class;


//making the cosine table
static double *cycle_makecostab(t_cycle *x){
	/*it looks like the original sickle was rounding up from the specified table length so COS_TABSIZE
	 originally wasn't even reflective of the actual table size... 
	 I've tried stepsize being twopi/(COS_TABSIZE) and also twopi/(COS_TABSIZE+1),.. in the former
	 case the last index would be cos(twopi) while in the latter it would be cos(twopi-1/(COS_TABSIZE+1))...
	 both seem to get no clicks,.. but if I make the wavetable exactly COS_TABSIZE indices,.. clicks....
	 so it has to be the way the tablesize is used in the perform methods... - DXK 
	 
	Matt's comments: yes, it was rounding up in case some other object using the makecostable function
	 asked for something that wasn't a power of 2.
	 
	 The function from sic made a table of COS_TABSIZE+1, and made sure that the waveform was symmetric.
	 In order to do this, the first quarter of the wave table was generated, then the 0 in the next index,
	 which needs to be exactly 0 (and cos(x) doesn't guarantee that even with a precise 2.f*M_PI). Then
	 the first quarter is copied in reverse order with a phase reversal, and then the first half is
	 copied exactly, in reverse order. This ensures that the guard point is the same as the 0th point, which
	 is exactly 1.0. The stepsize has to be twopi/(COS_TABSIZE) to get this to work, since you want a full
	 cycle through the first COS_TABSIZE, and then the beginning of a new cycle at the guard point.
	 
	 One last thing - since this table is exactly the same for each instance of [cycle~], it can be moved
	 out and accessed in one place by all instances, as in [osc~], rather than having each instance own its
	 own table. This saves memory (not that it is breaking the bank), but more importantly load time. So when
	 we call this function it either makes the table and returns a pointer, or if it's already been made it
	 just returns the pointer.
	*/
	 
	static double *costable; // stays allocated as long as pd is running
	 
	if (costable)
	{
		return costable;
	}
	double twopi = 2.0 * M_PI;
	int ndx, sz = COS_TABSIZE;
	int cnt = sz + 1;
	costable = getbytes(cnt * sizeof(*costable));
	double *tp = costable;
	int i;
	double phase = 0, phsinc = twopi / sz;
	if (costable)
	{
		i = sz/4;
		while (i--)
	    {
			*tp++ = cos(phase);
			phase += phsinc;		
	    }
	    *tp++ = 0;
	    i = sz/4;
	    while (i--)
	    {
	    	*tp++ = -costable[i];
	    }
	    i = sz/2;
	    while (i--)
	    {
	    	*tp++ = costable[i];
	    }
	}
	else
	{
		pd_error(x, "could not make cosine table");
	}
	return costable;
}


static t_word *cycle_fetcharray(t_cycle * x, int * tabsz, int indsp){
	//modifying old vefl_get a bit - DXK
	t_symbol * name = x->x_name;
	if (name && name != &s_){
		t_garray *ap = (t_garray *)pd_findbyclass(name, garray_class);
	if (ap){
			int tabsize;
			t_word *vec;
			if (garray_getfloatwords(ap, &tabsize, &vec)){
				if (indsp)
				{
					garray_usedindsp(ap);
				}
				if (tabsz)
				{
					*tabsz = tabsize;
				}
				return (vec);
			}
			else 
			{ /* always complain */
				pd_error(x, "bad template of array '%s'", name->s_name);
			}
		}
		else if (x)
		{
			pd_error(x, "no such array '%s'", name->s_name);
		}
	}
	return (0);

}

static void cycle_gettable(t_cycle *x)
{   
    x->x_table = 0;
    int cycle_tabsize = x->x_cycle_tabsize;
    t_word *table = 0;
    int tabsize;
    
    if (x->x_name){	
		table = cycle_fetcharray(x, &tabsize, 1);
	};
		
	/* CHECKED buffer is copied */
	if (table)
    {
    	if (x->x_use_all) cycle_tabsize = x->x_cycle_tabsize = tabsize;
    	int usertable_tabsize = x->x_usertable_tabsize;
	    int tablePtr, oldbytes;
	    int samplesFromTable = tabsize - x->x_offset;
	    int samplesToCopy = samplesFromTable < cycle_tabsize ?
		samplesFromTable : cycle_tabsize;
		
		/*  If requested cycle buffer size is larger than currently allocated,
			then resize the buffer, or if we're still using the builtin,
			get a new buffer on the heap. */
		if (cycle_tabsize > usertable_tabsize)
		{
			oldbytes = (usertable_tabsize + 1) * sizeof(*x->x_table);
			usertable_tabsize = x->x_usertable_tabsize = x->x_cycle_tabsize;
			if (x->x_usertable == x->x_usertableini)
			{
				if(!(x->x_usertable =
					getbytes((usertable_tabsize + 1) * sizeof(*x->x_usertable))))
				{
					x->x_usertable = x->x_usertableini;
					x->x_cycle_tabsize = x->x_usertable_tabsize = PDCYCYCLE_TABSIZE;
					pd_error(x,"unable to resize buffer; using size %d",PDCYCYCLE_TABSIZE);
				}
				else
				{
					//post("getting new table of size %d", cycle_tabsize);
					x->x_usertable_tabsize = cycle_tabsize;
				}
			}
			else
			{
				t_float *tmp;
				if(!(tmp =
					resizebytes(x->x_usertable, oldbytes,
						(usertable_tabsize + 1) * sizeof(*x->x_usertable))))
				{
					freebytes(x->x_usertable, oldbytes);
					x->x_usertable = x->x_usertableini;
					x->x_cycle_tabsize = x->x_usertable_tabsize = PDCYCYCLE_TABSIZE;
					pd_error(x,"unable to resize buffer; using size %d",PDCYCYCLE_TABSIZE);
				}
				else
				{
					x->x_usertable = tmp;
					x->x_usertable_tabsize = x->x_cycle_tabsize;
				}
			}
		}
	    // copy the internal table from the external one as far as 
	    // its size permits and fill the rest with zeroes.
	    for (tablePtr = 0; tablePtr < cycle_tabsize; tablePtr++)
	    {
		if (samplesToCopy > 0) 
		{
		    x->x_usertable[tablePtr] = 
			table[tablePtr + x->x_offset].w_float;
		    samplesToCopy--;
		}
		else 
		{
		    x->x_usertable[tablePtr] = 0;
		}
	    }
	    x->x_usertable[tablePtr] = (samplesFromTable > 0) ?
		table[x->x_offset].w_float : 0;

	    x->x_table = x->x_usertable;
	    /* CHECKED else no complaint */
	}
    else 
    {
    	if (x->x_name)
    	{
			pd_error(x, "using cosine table");
    	}
    	x->x_name = 0;
    	x->x_cycle_tabsize = COS_TABSIZE;
    }
}

static void cycle_set(t_cycle *x, t_symbol *s, t_floatarg f)
{
	x->x_use_all = 0;
	x->x_offset = 0;
	x->x_cycle_tabsize = PDCYCYCLE_TABSIZE;
    if (s && s != &s_)
    {
	x->x_name = s;
	if ((x->x_offset = (int)f) < 0)
	    x->x_offset = 0;
    }
    else x->x_name = 0;
    cycle_gettable(x);
}

static void cycle_setall(t_cycle *x, t_symbol *s)
{
	x->x_use_all = 1;
	x->x_offset = 0;
    if (s && s != &s_)
		x->x_name = s;
    else x->x_name = 0;
    cycle_gettable(x);
}

static void cycle_buffer_offset(t_cycle *x, t_floatarg f)
{
    if ((x->x_offset = (int)f) < 0)
    x->x_offset = 0;
    cycle_gettable(x);
}


static void cycle_set_buffersize(t_cycle *x, t_floatarg f)
{
	if (f==0.)
	{
		x->x_use_all = 0;
		x->x_cycle_tabsize = PDCYCYCLE_TABSIZE;
	}
	else if (f == -1.)
	{
		x->x_use_all = 1;
	}
	else if (f == (1 << ilog2(f)) && f <= 65536. && f >= 16)
	{
		x->x_use_all = 0;
		x->x_cycle_tabsize = f;
	}
	else
	{
		pd_error(x, "buffer_sizeinsamps must be a power of two from 16 to 65536");
		return;
	}
}

static void cycle_buffer_sizeinsamps(t_cycle *x, t_floatarg f)
{

	cycle_set_buffersize(x, f);
	cycle_gettable(x);
}

static void cycle_frequency(t_cycle *x, t_floatarg f)
{
    x->x_freq = f;
}

static void cycle_phase(t_cycle *x, t_floatarg f)
{
    x->x_init_phase = f;
}

static t_int *cycle_perform(t_int *w)
{
	t_cycle *x = (t_cycle *)(w[1]);
	int nblock = (int)(w[2]);
	t_float *in1 = (t_float *)(w[3]);
	t_float *in2 = (t_float *)(w[4]);
	t_float *out = (t_float *)(w[5]);
	t_float *tab = x->x_table;
	double  *costab = x->x_costable;
	double dphase = x->x_phase;
	double conv = x->x_conv;
	double wrapphase, tabphase, frac;
	t_float f1, f2, freq, phasein;
	double df1, df2;
	int intphase, index;
	int cycle_tabsize = x->x_cycle_tabsize;
	
	while (nblock--)
	{
		freq = *in1++;
		phasein = *in2++;
		wrapphase = dphase + phasein;
		if (wrapphase>=1.) 
		{
			intphase = (int)wrapphase;
			wrapphase -= intphase;
		}
		else if (wrapphase<=0.) 
		{
			intphase = (int)wrapphase;
			intphase--;
			wrapphase -= intphase;
		}
		
		tabphase = wrapphase * cycle_tabsize;
		index = (int)tabphase;
		frac = tabphase - index;
		if (x->x_name)
		{
			f1 = tab[index++];
			f2 = tab[index];
			*out++ = f1 + frac * (f2 - f1);
		}
		else
		{
			df1 = costab[index++];
			df2 = costab[index];
			*out++ = (t_float) (df1 + frac * (df2 - df1));
		}
		dphase += freq * conv ;
	}
	
	if (dphase>=1.)
	{
		intphase = (int)dphase;
		dphase -= intphase;
	}
	else if (dphase<=0.)
	{
		intphase = (int)dphase;
		intphase--;
		dphase -= intphase;
	}
	x->x_phase = dphase;
	return (w + 6);
}
	
static void cycle_dsp(t_cycle *x, t_signal **sp)
{
    cycle_gettable(x);
    x->x_conv = 1.0 / sp[0]->s_sr;
    x->x_phase = x->x_init_phase;
    pd_float((t_pd *)x->x_phaselet, 0.);
    //post("cycle tabsize = %d", x->x_cycle_tabsize);
    dsp_add(cycle_perform, 5, x, sp[0]->s_n,
	    sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec);
}

static void *cycle_new(t_symbol *s, int argc, t_atom *argv)
{
    t_cycle *x = (t_cycle *)pd_new(cycle_class);
    
	t_symbol * name;
	t_float phase, freq, offset, bufsz;

	phase = PDCYCYCLE_PHASE;
	freq = PDCYCYCLE_FREQ;
	offset = PDCYCYCLE_OFFSET;
	bufsz = PDCYCYCLE_TABSIZE;
	int argnum = 0;
	int anamedef = 0; //flag if array name is defined
	int pastargs = 0; //flag if attributes are specified, then don't accept array name anymore
	int bufferattrib = 0; //flag if @buffer attribute is set; needs to default to whole buffer
	int buffersizeattrib = 0; // flag if @buffer_sizeinsamps attribute is set
	while(argc > 0){
		if(argv -> a_type == A_FLOAT){
			if(!pastargs){
				t_float argval = atom_getfloatarg(0, argc, argv);
				switch(argnum){
					case 0:
						freq = argval;
						break;
					case 1:
						offset = argval;
						break;
					default:
						break;
					};
					argnum++;
				};
				argc--;
				argv++;
			}

		else if(argv -> a_type == A_SYMBOL){
			t_symbol * curarg = atom_getsymbolarg(0, argc, argv);
			if(strcmp(curarg->s_name, "@buffer")==0){
				if(argc >= 2){
					if((argv+1) -> a_type == A_SYMBOL){
						name = atom_getsymbolarg(1, argc, argv);
						argc-=2;
						argv+=2;
						anamedef = 1;
						pastargs = 1;
						bufferattrib = 1;
					}
					else{
						goto errstate;
					};
				}
				else{
					goto errstate;
				};
			}
			else if(strcmp(curarg->s_name, "@buffer_offset")==0){
				if(argc >= 2){
					offset = atom_getfloatarg(1, argc, argv);
					argc-=2;
					argv+=2;
					pastargs = 1;
				}
				else{
					goto errstate;
				};
			}
			else if(strcmp(curarg->s_name, "@buffer_sizeinsamps")==0){
				if(argc >= 2){
					bufsz = atom_getfloatarg(1, argc, argv);
					argc-=2;
					argv+=2;
					pastargs = 1;
					buffersizeattrib = 1;
				}
				else{
					goto errstate;
				};
			}
			else if(strcmp(curarg->s_name, "@phase")==0){
				if(argc >= 2){
					phase = atom_getfloatarg(1, argc, argv);
					argc-=2;
					argv+=2;
					pastargs = 1;
				}
				else{
					goto errstate;
				};
			}
			else if(strcmp(curarg->s_name, "@frequency")==0){
				if(argc >= 2){
					freq  = atom_getfloatarg(1, argc, argv);
					argc-=2;
					argv+=2;
					pastargs = 1;
				}
				else{
					goto errstate;
				};
			}
			else if(!pastargs){
				//specifying buffer name through argt
					name = curarg;
					argc--;
					argv++;
					anamedef = 1;
			}
			else{
				goto errstate;
			};
		}
		else{
			goto errstate;
		};
	};

    x->x_costable = cycle_makecostab(x);
    x->x_usertable = x->x_usertableini;
    x->x_usertable_tabsize = PDCYCYCLE_TABSIZE;
	x->x_phaselet = inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
	x->x_outlet = outlet_new(&x->x_obj, gensym("signal"));
	if(offset < 0){
		offset = 0;
	};
    x->x_offset = offset;
    if(anamedef){
		x->x_name = name;
    	}
    else{
		x->x_name = 0;
	};
	x->x_freq = freq;
    x->x_table = 0;
    x->x_init_phase = phase;
    x->x_conv = 0.;
    cycle_set_buffersize(x, bufsz);
    if(bufferattrib){
    	if (!buffersizeattrib)
    		x->x_use_all = 1;
    }
    return (x);
	errstate:
		pd_error(x, "cycle~: improper args");
		return NULL;
}

static void *cycle_free(t_cycle *x)
{
	if (x->x_usertable != x->x_usertableini){
		freebytes(x->x_usertable, (x->x_usertable_tabsize + 1) * sizeof(*x->x_usertable));
	};
	//free(x->x_costable);
	outlet_free(x->x_outlet);
	inlet_free(x->x_phaselet);
	return (void *)x;
}

void cycle_tilde_setup(void)
{    
    cycle_class = class_new(gensym("cycle~"), (t_newmethod)cycle_new, (t_method)cycle_free,
        sizeof(t_cycle), 0, A_GIMME, 0);
	CLASS_MAINSIGNALIN(cycle_class, t_cycle, x_freq); 
	class_addmethod(cycle_class, (t_method)cycle_dsp, gensym("dsp"), A_CANT, 0);
    class_addmethod(cycle_class, (t_method)cycle_set,gensym("set"),
        A_DEFSYMBOL, A_DEFFLOAT, 0);
	class_addmethod(cycle_class, (t_method)cycle_setall, gensym("buffer"),
		A_DEFSYMBOL, 0);
    class_addmethod(cycle_class, (t_method)cycle_buffer_offset,gensym("buffer_offset"),
        A_DEFFLOAT, 0);
    class_addmethod(cycle_class, (t_method)cycle_buffer_sizeinsamps,gensym("buffer_sizeinsamps"),
        A_DEFFLOAT, 0);
    class_addmethod(cycle_class, (t_method)cycle_setall,gensym("setall"),
        A_DEFSYMBOL, 0);
    class_addmethod(cycle_class, (t_method)cycle_frequency, gensym("frequency"), A_DEFFLOAT, 0);
    class_addmethod(cycle_class, (t_method)cycle_phase, gensym("phase"), A_DEFFLOAT, 0);
}
