/*!
    \defgroup MINTS libmints: Integral library
    \ingroup MINTS
*/

#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include <libciomr/libciomr.h>
#include <libchkpt/chkpt.hpp>
#include <libparallel/parallel.h>
#include <psifiles.h>
#include <chkpt_params.h>


#include "vector3.h"
#include "molecule.h"
#include "basisset.h"
#include "integral.h"
#include "symmetry.h"
#include "gshell.h"
#include "factory.h"
#include "basisset_parser.h"
#include "pointgrp.h"
#include "wavefunction.h"
#include "sobasis.h"

using namespace std;
using namespace psi;
using namespace boost;

once_flag BasisSet::initialized_shared_ = BOOST_ONCE_INIT;

std::vector<Vector3> BasisSet::exp_ao[LIBINT_MAX_AM];

BasisSet::BasisSet() :
    max_nprimitives_(0), shell_first_basis_function_(NULL), shell_first_ao_(NULL), shell_center_(NULL),
    max_stability_index_(0), uso2ao_(NULL), uso2bf_(NULL)
{
    call_once(initialize_singletons, initialized_shared_);
}

BasisSet::BasisSet(shared_ptr<Chkpt> chkpt, std::string basiskey) :
    max_nprimitives_(0), shell_first_basis_function_(NULL), shell_first_ao_(NULL), shell_center_(NULL),
    max_stability_index_(0), uso2ao_(NULL), uso2bf_(NULL),
    molecule_(new Molecule)
{
    call_once(initialize_singletons, initialized_shared_);

    // This requirement holds no matter what.
    if(Communicator::world->me() == 0)
        puream_ = chkpt->rd_puream(basiskey.c_str()) ? true : false;
    if(Communicator::world->nproc() > 1)
        Communicator::world->raw_bcast(&puream_, sizeof(bool), 0);

    // Initialize molecule, retrieves number of center and geometry
    molecule_->init_with_chkpt(chkpt);
    
    // Obtain symmetry information from the molecule.
    shared_ptr<PointGroup> pg = molecule_->find_point_group();
    molecule_->set_point_group(pg);
    molecule_->form_symmetry_information();

    // Initialize the shells
    initialize_shells(chkpt, basiskey);
}

BasisSet::~BasisSet()
{
    if (shell_first_basis_function_)
        Chkpt::free(shell_first_basis_function_);
    if (shell_first_ao_)
        Chkpt::free(shell_first_ao_);
    if (shell_center_)
        Chkpt::free(shell_center_);
    if (uso2ao_)
        Chkpt::free(uso2ao_);
    if (uso2bf_)
        Chkpt::free(uso2bf_);
}

void BasisSet::initialize_singletons()
{
    // Populate the exp_ao arrays
    int ao;
    for (int l=0; l<LIBINT_MAX_AM; ++l) {
        ao = 0;
        for (int i=0; i<=l; ++i) {
            int x = l-i;
            for (int j=0; j<=i; ++j) {
                int y = i-j;
                int z = j;

                Vector3 xyz_ao(x, y, z);
                BasisSet::exp_ao[l].push_back(xyz_ao);

                ao++;
            }
        }
    }
}

void BasisSet::build_ao_transformation_matrix()
{
    // Obtain the character table of the molecule we are working with.
    CharacterTable char_table = molecule_->point_group()->char_table();
    int nirrep = char_table.nirrep();

    double ***ao_type_transmat = new double**[LIBINT_MAX_AM];
    for (int l=0; l<LIBINT_MAX_AM; ++l)
        ao_type_transmat[l] = block_matrix(nirrep, ioff[l+1]);

    // Apply the operations to the different atomic orbitals we'll find
    for (int l=0; l<LIBINT_MAX_AM; ++l) {
        for (int h=0; h<nirrep; ++h) {
            SymmetryOperation& symop = char_table.symm_operation(h);
            for (int i=0; i<ioff[l+1]; ++i) {
                ao_type_transmat[l][h][i] = pow(symop[0][0], exp_ao[l][i][0]) *
                                            pow(symop[1][1], exp_ao[l][i][1]) *
                                            pow(symop[2][2], exp_ao[l][i][2]);
            }
        }
    }

    int **num_cart_so = init_int_matrix(LIBINT_MAX_AM, nirrep);
    int **ao_type_irr = new int*[LIBINT_MAX_AM];
    for (int l=0; l<LIBINT_MAX_AM; ++l) {
        ao_type_irr[l] = new int[ioff[l+1]];
        memset(ao_type_irr[l], 0, sizeof(int)*ioff[l+1]);
    }

    for (int l=0; l<LIBINT_MAX_AM; ++l) {
        for (int ao=0; ao<ioff[l+1]; ++ao) {
            for (int h=0; h<nirrep; ++h) {
                int coeff=0;
                for (int symop = 0; symop<nirrep; ++symop) {
//                    coeff += ao_type_transmat[l][symop][ao] * char_table.gamma(h).p();
//                    coeff += ao_type_transmat[l][symop][ao] * char_table.gamma(h).character(symop);
                }
                if (coeff != 0) {
                    ao_type_irr[l][ao] = h;
                    num_cart_so[l][h]++;
                }
            }
        }
    }

    fprintf(outfile, "  Transformation matrices :\n\n");
    for(int l=0; l<LIBINT_MAX_AM; l++) {
        fprintf(outfile,"   l = %d\n",l);
        for(int i=0; i<nirrep; i++) {
            fprintf(outfile,"Symmetry Operation %d\n",i);
            for(int j=0; j<ioff[l+1]; j++)
                fprintf(outfile," %d  %lf\n", j+1, ao_type_transmat[l][i][j]);
            fprintf(outfile,"\n");
        }
        fprintf(outfile,"\n");
    }
}

void BasisSet::initialize_shells(shared_ptr<Chkpt> chkpt, std::string& basiskey)
{
    if(Communicator::world->me() == 0) {
        // Initialize some data from checkpoint.
        nshells_      = chkpt->rd_nshell(basiskey.c_str());
        nprimitives_  = chkpt->rd_nprim(basiskey.c_str());
        nao_          = chkpt->rd_nao(basiskey.c_str());

        // Psi3 only allows either all Cartesian or all Spherical harmonic
        nbf_          = chkpt->rd_nso(basiskey.c_str());
        max_am_       = chkpt->rd_max_am(basiskey.c_str());
    }

    if(Communicator::world->nproc() > 1) {
        Communicator::world->raw_bcast(&nshells_, sizeof(int), 0);
        Communicator::world->raw_bcast(&nprimitives_, sizeof(int), 0);
        Communicator::world->raw_bcast(&nao_, sizeof(int), 0);
        Communicator::world->raw_bcast(&nbf_, sizeof(int), 0);
        Communicator::world->raw_bcast(&max_am_, sizeof(int), 0);
    }

    if(Communicator::world->me() == 0) {
        uso2ao_       = chkpt->rd_usotao(basiskey.c_str());
        uso2bf_       = chkpt->rd_usotbf(basiskey.c_str());
    }
    else {
        uso2ao_ = block_matrix(nbf_, nao_);
        uso2bf_ = block_matrix(nbf_, nbf_);
    }

    if(Communicator::world->nproc() > 1) {
        Communicator::world->raw_bcast(&(uso2ao_[0][0]), nbf_*nao_*sizeof(double), 0);
        Communicator::world->raw_bcast(&(uso2bf_[0][0]), nbf_*nbf_*sizeof(double), 0);
    }



    simple_mat_uso2ao_ = shared_ptr<SimpleMatrix>(new SimpleMatrix("Unique SO to AO transformation matrix", nbf_, nao_));
    simple_mat_uso2ao_->set(uso2ao_);
    // simple_mat_uso2ao_.print();

    simple_mat_uso2bf_ = shared_ptr<SimpleMatrix>(new SimpleMatrix("Unique SO to BF transformation matrix", nbf_, nbf_));
    simple_mat_uso2bf_->set(uso2bf_);
    // simple_mat_uso2bf_.print();

    int *shell_am;
    int *shell_num_prims;
    double *exponents;
    double **ccoeffs;
    int *shell_fprim;
    if(Communicator::world->me() == 0) {
        // Retrieve angular momentum of each shell (1=s, 2=p, ...)
        shell_am = chkpt->rd_stype(basiskey.c_str());

        // Retrieve number of primitives per shell
        shell_num_prims = chkpt->rd_snumg(basiskey.c_str());

        // Retrieve exponents of primitive Gaussians
        exponents = chkpt->rd_exps(basiskey.c_str());

        // Retrieve coefficients of primitive Gaussian
        ccoeffs = chkpt->rd_contr_full(basiskey.c_str());

        // Retrieve pointer to first primitive in shell
        shell_fprim = chkpt->rd_sprim(basiskey.c_str());

        // Retrieve pointer to first basis function in shell
        shell_first_basis_function_ = chkpt->rd_sloc_new(basiskey.c_str());

        // Retrieve pointer to first AO in shell
        shell_first_ao_ = chkpt->rd_sloc(basiskey.c_str());

        // Retrieve location of shells (which atom it's centered on)
        shell_center_ = chkpt->rd_snuc(basiskey.c_str());
    }
    else {
        shell_am = init_int_array(nshells_);
        shell_num_prims = init_int_array(nshells_);
        exponents = init_array(nprimitives_);
        ccoeffs = block_matrix(nprimitives_, MAXANGMOM);
        shell_fprim = init_int_array(nshells_);
        shell_first_basis_function_ = init_int_array(nshells_);
        shell_first_ao_ = init_int_array(nshells_);
        shell_center_ = init_int_array(nshells_);
    }

    if(Communicator::world->nproc() > 1) {
        Communicator::world->raw_bcast(&(shell_am[0]), nshells_*sizeof(int), 0);
        Communicator::world->raw_bcast(&(shell_num_prims[0]), nshells_*sizeof(int), 0);
        Communicator::world->raw_bcast(&(exponents[0]), nprimitives_*sizeof(double), 0);
        Communicator::world->raw_bcast(&(ccoeffs[0][0]), nprimitives_*MAXANGMOM*sizeof(double), 0);
        Communicator::world->raw_bcast(&(shell_fprim[0]), nshells_*sizeof(int), 0);
        Communicator::world->raw_bcast(&(shell_first_basis_function_[0]), nshells_*sizeof(int), 0);
        Communicator::world->raw_bcast(&(shell_first_ao_[0]), nshells_*sizeof(int), 0);
        Communicator::world->raw_bcast(&(shell_center_[0]), nshells_*sizeof(int), 0);
    }

    // Initialize SphericalTransform
    for (int i=0; i<=max_am_; ++i) {
        sphericaltransforms_.push_back(SphericalTransform(i));
    }

    // Initialize SOTransform
    sotransform_ = shared_ptr<SOTransform>(new SOTransform);
    sotransform_->init(nshells_);

    int *so2symblk = new int[nbf_];
    int *so2index  = new int[nbf_];

    int *sopi;
    int nirreps;
    if(Communicator::world->me() == 0) {
        sopi = chkpt->rd_sopi(basiskey.c_str());
        nirreps = chkpt->rd_nirreps();
    }
    if(Communicator::world->nproc() > 1)
        Communicator::world->raw_bcast(&nirreps, sizeof(int), 0);
    if(Communicator::world->me() != 0)
        sopi = init_int_array(nirreps);
    if(Communicator::world->nproc() > 1)
        Communicator::world->raw_bcast(&(sopi[0]), nirreps*sizeof(int), 0);

    // Create so2symblk and so2index
    int ij = 0; int offset = 0;
    for (int h=0; h<nirreps; ++h) {
        for (int i=0; i<sopi[h]; ++i) {
            so2symblk[ij] = h;
            so2index[ij] = ij-offset;

            ij++;
        }
        offset += sopi[h];
    }

    // Currently all basis sets are treated as segmented contractions
    // even though GaussianShell is generalized (well not really).
    int ao_start = 0;
    int puream_start = 0;

    for (int i=0; i<nshells_; ++i) {
        int am;
        am = shell_am[i] - 1;

        // For some awful reason input stored shell_center_ as 1-based....let's fix this
        shell_center_[i]--;

        int fprim = shell_fprim[i] - 1;
        int nprims = shell_num_prims[i];
        Vector3 center = molecule_->xyz(shell_center_[i]);
        double *cc = new double[nprims];
        for (int p=0; p<nprims; ++p) {
            cc[p] = ccoeffs[fprim+p][am];
        }

        // Construct a new shell. GaussianShell copies the data to new memory
        shells_.push_back(shared_ptr<GaussianShell>(new GaussianShell));
        shells_[i]->init(nprims, &(exponents[fprim]), am,
            puream_ ? Pure : Cartesian, cc, shell_center_[i], center,
            puream_start);

        if (nprims > max_nprimitives_)
            max_nprimitives_ = nprims;

        delete[] cc;

        // OK, for a given number of AO functions in a shell INT_NCART(am)
        // beginning at column ao_start go through all rows finding where this
        // AO function contributes to an SO.
        for (int ao = 0; ao < INT_NCART(am); ++ao) {
            int aooffset = ao_start + ao;
            for (int so = 0; so < nbf_; ++so) {
                if (fabs(uso2ao_[so][aooffset]) >= 1.0e-14)
                    sotransform_->add_transform(i, so2symblk[so], so2index[so], uso2ao_[so][aooffset], ao, so);
            }
        }

        // Shift the ao starting index over to the next shell
        ao_start += INT_NCART(am);
        puream_start += INT_NFUNC(puream_, am);
    }

    delete[] so2symblk;
    delete[] so2index;
    Chkpt::free(sopi);
    Chkpt::free(ccoeffs);
    Chkpt::free(exponents);
    Chkpt::free(shell_am);
    Chkpt::free(shell_num_prims);
    Chkpt::free(shell_fprim);
}

void BasisSet::print(FILE *out) const
{
    fprintf(out, "  Basis Set\n");
    fprintf(out, "    Number of shells: %d\n", nshell());
    fprintf(out, "    Number of basis function: %d\n", nbf());
    fprintf(out, "    Number of Cartesian functions: %d\n", nao());
    fprintf(out, "    Spherical Harmonics?: %s\n", has_puream() ? "true" : "false");
    fprintf(out, "    Max angular momentum: %d\n\n", max_am());

//    fprintf(out, "    Shells:\n\n");
//    for (int s=0; s<nshell(); ++s)
//        shells_[s]->print(out);
}

shared_ptr<GaussianShell> BasisSet::shell(int si) const
{
    #ifdef DEBUG
    assert(si < nshell());
    #endif
    return shells_[si];
}

shared_ptr<GaussianShell> BasisSet::shell(int center, int si) const
{
    #ifdef DEBUG
    assert(si < nshell());
    #endif
    return shells_[center_to_shell_[center] + si];
}

shared_ptr<BasisSet> BasisSet::zero_basis_set()
{
    shared_ptr<BasisSet> new_basis(new BasisSet());

    // Setup all the parameters needed for a zero basis set
    new_basis->shell_first_basis_function_ = NULL;
    new_basis->shell_first_ao_ = NULL;
    new_basis->shell_center_ = new int[1];
    new_basis->shell_center_[0] = 0;
    new_basis->uso2ao_ = NULL;
    new_basis->max_nprimitives_ = 1;
    new_basis->max_stability_index_ = 0;
    new_basis->max_am_ = 0;

    new_basis->puream_ = false;

    // Add "ghost" atom to the molecule for this basis
    new_basis->molecule_ = shared_ptr<Molecule>(new Molecule);
    // Ghost atoms are now handled differently, they are not added to the normal xyz information array,
    // but to the fxyz array.
    new_basis->molecule_->add_atom(0, 0.0, 0.0, 0.0);
    Vector3 center = new_basis->molecule_->fxyz(0);

    new_basis->nshells_ = 1;
    new_basis->nprimitives_ = 1;
    new_basis->nao_ = 1;
    new_basis->nbf_ = 1;
    new_basis->uso2ao_ = Chkpt::matrix<double>(1, 1);
    new_basis->uso2ao_[0][0] = 1.0;
    new_basis->uso2bf_ = Chkpt::matrix<double>(1, 1);
    new_basis->uso2bf_[0][0] = 1.0;

    // Create shell array
    new_basis->shells_.push_back(shared_ptr<GaussianShell>(new GaussianShell));

    // Spherical and SO-transforms are expected even if not used.
    new_basis->sphericaltransforms_.push_back(SphericalTransform(0));
    new_basis->sotransform_ = shared_ptr<SOTransform>(new SOTransform);
    new_basis->sotransform_->init(1);

    // Create out basis set arrays
    // null basis set
    int am   = 0;
    double e = 0.0;
    double c = 1.0;

    // Add the null-s-function
    new_basis->shells_[0]->init(1, &e, am, Cartesian, &c, 0, center, 0, Normalized);

    // Add s-function SO transform.
    new_basis->sotransform_->add_transform(0, 0, 0, 1.0, 0, 0);

    return new_basis;
}

shared_ptr<BasisSet> BasisSet::construct(const shared_ptr<BasisSetParser>& parser,
        const shared_ptr<Molecule>& mol,
        const std::string& basisname)
{
    // Construct vector with the same basis name for each element and pass to
    // the other version of construct
    vector<string> basisnames;

    // Update geometry in molecule, if there is a problem an exception is thrown
    mol->update_geometry();

    for (int i=0; i<mol->natom(); ++i) {
//        printf("adding %s to basisnames\n", basisname.c_str());
        basisnames.push_back(basisname);
    }

    return construct(parser, mol, basisnames);
}

shared_ptr<BasisSet> BasisSet::construct(const shared_ptr<BasisSetParser>& parser,
        const shared_ptr<Molecule>& mol,
        const std::vector<std::string>& basisnames)
{
    shared_ptr<BasisSet> basisset(new BasisSet);

    // Update geometry in molecule, if there is a problem an exception is thrown.
    mol->update_geometry();

    basisset->molecule_ = mol;
    parser->parse(basisset, basisnames);

    for (int i=0; i<=basisset->max_am(); ++i)
        basisset->sphericaltransforms_.push_back(SphericalTransform(i));

    // Form the spherical transform (simple bf->ao)
    // Initialize SOTransform
    basisset->sotransform_ = shared_ptr<SOTransform>(new SOTransform);
    basisset->sotransform_->init(basisset->nshell());

    int so_global=0;
    int ao_global=0;
    for (int i=0; i < basisset->nshell(); ++i) {
        SphericalTransform st(basisset->shell(i)->am());
        SphericalTransformIter iter(st);

        ao_global = basisset->shell_to_function(i);
        so_global = basisset->shell_to_basis_function(i);

        for (iter.first(); !iter.is_done(); iter.next()) {
            basisset->sotransform_->add_transform(i, 0, iter.pureindex() + so_global, iter.coef(),
                                                  iter.cartindex(), iter.pureindex() + so_global);
        }
    }

    return basisset;
}

void BasisSet::refresh()
{
    // Reset data to initial values
    nshells_ = shells_.size();
    nprimitives_ = 0;
    nao_ = 0;
    nbf_ = 0;
    max_am_ = 0;
    max_nprimitives_ = 0;
    puream_ = false;

    if (shell_first_basis_function_)
        delete[] shell_first_basis_function_;
    if (shell_first_ao_)
        delete[] shell_first_ao_;
    if (shell_center_)
        delete[] shell_center_;

    shell_first_basis_function_ = new int[nshells_];
    shell_first_ao_             = new int[nshells_];
    shell_center_               = new int[nshells_];

    center_to_nshell_.clear(); center_to_nshell_.resize(molecule_->natom(), 0);
    center_to_shell_.clear();  center_to_shell_.resize(molecule_->natom(), 0);
    center_to_shell_[0] = 0;

    int current_center = 0;

    for (int i=0; i<nshells_; ++i) {
        shell_center_[i]   = shells_[i]->ncenter();
        shell_first_ao_[i] = nao_;
        shell_first_basis_function_[i] = nbf_;
        shells_[i]->set_function_index(nbf_);

        center_to_nshell_[shell_center_[i]]++;
        if (current_center != shell_center_[i]) {
            center_to_shell_[shell_center_[i]] = i;
            current_center = shell_center_[i];
        }

        nprimitives_ += shells_[i]->nprimitive();
        nao_         += shells_[i]->ncartesian();
        nbf_         += shells_[i]->nfunction();

        if (max_am_ < shells_[i]->am())
            max_am_ = shells_[i]->am();

        if (max_nprimitives_ < shells_[i]->nprimitive())
            max_nprimitives_ = shells_[i]->nprimitive();

        if (puream_ == false && shells_[i]->is_pure())
            puream_ = true;
    }

    function_to_shell_.resize(nbf());
    int ifunc = 0;
    for (int i=0; i<nshells_; ++i) {
        int nfun = shell(i)->nfunction();
        for (int j=0; j<nfun; ++j) {
            function_to_shell_[ifunc] = i;
            ifunc++;
        }
    }
}

shared_ptr<BasisSet> BasisSet::atomic_basis_set(int fcenter)
{
    //May only work in C1!!!!
    //Construct a blank BasisSet on the heap
    shared_ptr<BasisSet> bas(new BasisSet);
    //Construct a blank Molecule on the heap
    shared_ptr<Molecule> mol(new Molecule);

    int Z = molecule_->fZ(fcenter);
    double x = molecule_->fx(fcenter);
    double y = molecule_->fy(fcenter);
    double z = molecule_->fz(fcenter);
    double mass = molecule_->fmass(fcenter);
    double charge = molecule_->fcharge(fcenter);
    std::string lab = molecule_->flabel(fcenter);
    char* label = new char[lab.length() + 1];
    strcpy(label,lab.c_str());

    //Put the atomic info into mol
    mol->add_atom(Z, x, y, z, label, mass, charge);

    //Assign the atomic molecule to bas
    bas->molecule_ = mol;

    int index_criteria = fcenter;

    //Go through shells in current basis set
    //Push shells that belong to fcenter
    //to bas
    int current_shells = 0;
    int shell_start = -1;
    int ao_start = 0;
    int so_start = 0;
    for (int i = 0; i<nshells_; i++) {
        if (shell_center_[i] < index_criteria) {
            ao_start += shells_[i]->ncartesian();
            so_start += shells_[i]->nfunction();
        }
        if (shell_center_[i] == index_criteria) {
            if (shell_start == -1)
                shell_start = i;
            shared_ptr<GaussianShell> shell(new GaussianShell);
            int nprm = shells_[i]->nprimitive();
            int am = shells_[i]->am();
            GaussianType harmonics = (shells_[i]->is_pure() ? Pure : Cartesian);
            int nc = 0; // In the atomic basis, always on the 0th atom
            int start = 0; //Will be reset later
            Vector3 center = shells_[i]->center();
            double* e = shells_[i]->exps();
            double* c = shells_[i]->coefs();

            shell->init(nprm,e,am, harmonics, c,nc,
              center,start);

            bas->shells_.push_back(shell);
            current_shells++;
        }
        if (shell_center_[i] > index_criteria)
            break;
    }

    // Initialize SphericalTransform
    for (int i=0; i<=max_am_; ++i) {
        bas->sphericaltransforms_.push_back(SphericalTransform(i));
    }
    //Initialize SOTransform
    bas->sotransform_ = shared_ptr<SOTransform>(new SOTransform);
    bas->sotransform_->init(current_shells);

    //Populate SOTransform
    for (int i = 0; i< current_shells; i++) {
        //OK, this is the SOTransformShell in the full basis
        SOTransformShell* full_so = sotransform_->aoshell(shell_start+i);
        //and it must go into the SOTransform in the atomic basis, and the offset in shell, ao, and so must be respected
        for (int ao = 0; ao < full_so->nfunc(); ao++) {
            //printf("AO %d, AOStart %d, SOStart%d, i %d, shell_start %d.\n",ao,ao_start,so_start,i,shell_start);
            bas->sotransform_->add_transform(i,full_so->func(ao)->irrep(),full_so->func(ao)->sofuncirrep()-so_start,full_so->func(ao)->coef(),full_so->func(ao)->aofunc(),full_so->func(ao)->sofunc()-so_start);
        }
        //ao_start += shells_[i+shell_start]->ncartesian();
        //so_start += shells_[i+shell_start]->nfunction();
    }

    //Setup the indexing in the atomic basis
    bas->refresh();

    //And ... return
    return bas;
}

SOTransformShell* BasisSet::so_transform(int i)
{
    return sotransform_->aoshell(i);
}

SphericalTransform& BasisSet::spherical_transform(int am)
{
    return sphericaltransforms_[am];
}

