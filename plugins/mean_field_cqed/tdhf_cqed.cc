#include <libplugin/plugin.h>
#include <psi4-dec.h>
#include <libparallel/parallel.h>
#include <liboptions/liboptions.h>
#include <libmints/mints.h>
#include <libpsio/psio.hpp>
#include "frozen_natural_orbitals.h"
#include<libciomr/libciomr.h>

#include "tdhf.h"

INIT_PLUGIN

using namespace boost;
using namespace psi;

namespace psi{ namespace tdhf_cqed {

extern "C" 
int read_options(std::string name, Options& options)
{
    if (name == "MEAN_FIELD_CQED"|| options.read_globals()) {
        /*- The amount of information printed to the output file -*/
        options.add_int("PRINT", 1);
        /*- total time in a.u. -*/
        options.add_double("TOTAL_TIME", 100.0);
        /*- time step in a.u. -*/
        options.add_double("TIME_STEP", 0.2);
        /*- pulse shape -*/
        options.add_str("LASER_SHAPE", "SIN_SQUARED", "SIN_SQUARED TRAPEZOID PI_PULSE CONTINUOUS GAUSSIAN");
        /*- transition dipole moment for pi pulse -*/
        options.add_double("LASER_TDPM", -0.415638122584);
        /*- amplitude of pulse in a.u.-*/
        options.add_double("LASER_AMP", 0.05);
        /*- frequency of pulse in a.u. (default is the 1 fs pulse) -*/
        options.add_double("LASER_FREQ", 0.1519829846);
        /*- width of pulse in a.u. (1 fs default) -*/
        options.add_double("LASER_TIME", 41.3414);
        /*- flag for linear-response absorption instead of
            real-time interaction with some external field -*/
        options.add_bool("LINEAR_RESPONSE",false);
        /*- get the spectrum for the molecule only -*/
        options.add_bool("MOLECULE_ONLY",false);
        /*- get the spectrum for the plasmon only -*/
        options.add_bool("PLASMON_ONLY",false);
        /*- polarization (default x+y+z). -*/
        options.add("POLARIZATION",new ArrayType());
        /*- plasmonic-molecule distance (bohr)-*/
        options.add("PLASMON_COORDINATES",new ArrayType());
        /*- plasmonic states -*/
        options.add_int("N_PLASMON_STATES", 1);
        /*- plasmon excitation energy (a.u.) -*/
        options.add("PLASMON_E", new ArrayType());
        /*- coupling energy (a.u.) -*/
        options.add_double("COUPLING_E", 10.8e-3/27.21138);
        /*- plasmon transition dipole moment (a.u.) -*/
        options.add("PLASMON_TDM",/* 2990.0/2.54175*/new ArrayType());
        /*- plasmon damping rate (a.u.) -*/
        options.add_double("PLASMON_DR", 150e-3/27.21138);
        /*- electron damping rate (a.u.) -*/
        options.add_double("ELECTRON_DR", 1e-6);
        /*- dielectric constant of the medium -*/
        options.add_double("EPSILON_M", 0.079577472);
    }

    return true;
}

void is_this_necessary(boost::shared_ptr<Wavefunction> wfn, Options & options);

extern "C" 
PsiReturnType mean_field_cqed(Options& options)
{
    int print = options.get_int("PRINT");

    tstart();

    // 3-index integrals (generated/read by fno class)
    boost::shared_ptr<DFFrozenNO> fno(new DFFrozenNO(Process::environment.wavefunction(),options));
    fno->ThreeIndexIntegrals();
    fno.reset();
    //boost::shared_ptr<TDHF> tdhf ( new TDHF(Process::environment.wavefunction(),options) );

    is_this_necessary(Process::environment.wavefunction(),options);

    tstop();

    return Success;
}

}} // End namespaces
