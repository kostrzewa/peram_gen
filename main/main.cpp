#include "tmLQCD.h"

#include "distillery.h"
#include "input_parms.h"

#include "mpi.h"

#include "unistd.h"

#include <cstdlib>

int main(int argc, char *argv[]){

  //MPI initialisation stuff
  int mpi_thread_provided;
  MPI_Init_thread(&argc, &argv, MPI_THREAD_SERIALIZED, &mpi_thread_provided);
  int numprocs = 0, myid = 0;
  MPI_Comm_size(MPI_COMM_WORLD, &numprocs);
  MPI_Comm_rank(MPI_COMM_WORLD, &myid);
  MPI_Group world_group;
  MPI_Comm_group( MPI_COMM_WORLD, &world_group );
  MPI_Comm mpi_comm_world_2;
  MPI_Comm_create( MPI_COMM_WORLD, world_group, &mpi_comm_world_2 );

  // Eigen functions will be called from multiple threads
  Eigen::initParallel();
  // but Eigen itself should run single-threaded
  Eigen::setNbThreads(1);

  // initialisation of the twisted mass stuff - MUST BE the first thing to do
  int verbose = 1; // set to 1 to make tmLQCD more verbose
  tmLQCD_invert_init(argc, argv, verbose, myid);
  MPI_Barrier(MPI_COMM_WORLD);

  // initialisation of distillery
  LapH::input_parameter param;
  param.parse_input_file(argc, argv);
  
  if( param.hack_clean && param.nb_rnd > 1 ){
    if( myid == 0 ){
      std::cout 
        << "You have chosen to run peram_gen with 'hack_clean = 1' "  << std::endl
        << "but have set a number of random vectors > 1" << std::endl
        << "The 'hack_clean' mode is a total and utter hack, clearing lots of memory before" << std::endl
        << "the perambulator is written to disk, thereby restricting the code to " << std::endl
        << "only being able to do one random vector at a time." << std::endl
        << "If you wish to do more than one random vector in a single job, set 'hack_clean = 0'" << std::endl
        << "and ensure that there is enough memory available for the perambulator write buffer" << std::endl
        << "to fit in addition to all the other allocations" << std::endl
        << "peram_gen will terminate now!" << std::endl;
      fflush(stdout);
    }
    tmLQCD_finalise();
    MPI_Finalize();
    exit(123);
  }

  if(myid == 0) {
    std::cout << "processing config: " << param.config << "\n" << std::endl;
  }
  MPI_Barrier(MPI_COMM_WORLD);

  int gauge_read = tmLQCD_read_gauge(param.config);
  MPI_Barrier(MPI_COMM_WORLD);
  
  if( gauge_read < 0 ){
    tmLQCD_finalise();
    printf("There was an error in tmLQCD_read_gauge!\n");
    MPI_Finalize();
    exit(222);
  }

  LapH::distillery dis;
  dis.initialise(param);
  MPI_Barrier(MPI_COMM_WORLD);

  // preparing source creation -------------------------------------------------
  size_t nb_of_inversions =  param.dilution_size_so[2];
    std::complex<double>** sources = new std::complex<double>*[nb_of_inversions];
    std::complex<double>** propagators_t0 = new std::complex<double>*[nb_of_inversions];
    std::complex<double>** propagators_t1 = new std::complex<double>*[nb_of_inversions];

  int length = 3*4*param.Lt*param.Ls*param.Ls*param.Ls/numprocs;

  // loop over random vectors
  for(size_t rnd_id = 0; rnd_id < param.nb_rnd; ++rnd_id) {
    // take some memory allocation overhead to reduce overall memory usage
    // these will be deleted before the perambulator is written (which
    // requires significant temporary storage)
    double mem_time = MPI_Wtime();
    for(size_t i = 0; i < nb_of_inversions; ++i){
      sources[i] = new std::complex<double>[length];
      propagators_t1[i] = new std::complex<double>[length];
      propagators_t0[i] = new std::complex<double>[length];
    }
    if( myid == 0 )
      printf("memory allocation took %f seconds\n", MPI_Wtime() - mem_time);


    std::complex<double>** propagators_temp;
    omp_set_num_threads(param.peram_gen_omp_num_threads);
    #pragma omp parallel
    {
      int num_threads = omp_get_num_threads();
      int thread_id   = omp_get_thread_num();
      
      // loop over all inversions
      for(size_t dil_t = 0; dil_t < param.dilution_size_so[0]; ++dil_t){
        for(size_t dil_e = 0; dil_e < param.dilution_size_so[1]; ++dil_e){
          // the first thread generates the sources and does the inversions
          // the second thread moves on to the barrier and waits      
          if( num_threads == 1 || thread_id == 0 ){
            
            double t0_time = omp_get_wtime();
            dis.create_source(dil_t,dil_e,sources);
            if(myid == 0)
              printf("create_source took %f seconds\n", omp_get_wtime() - t0_time);
            
            for(size_t dil_d = 0; dil_d < param.dilution_size_so[2]; ++dil_d){        

              if(myid == 0) 
                std::cout << "\t\nDoing inversions at: t = " << dil_t << "\t e = " << dil_e 
                          << "\t d = " << dil_d << "\n" << std::endl;
  
              // tmLQCD can also write the propagator, if requested
              unsigned int op_id = 0;
              unsigned int write_prop = 0;
              
              t0_time = omp_get_wtime();
#ifndef PG_QUDA_DIRECT
              {  // open a block for convenience 
#else
              if(param.quda_direct){
                // use direct passthrough to QUDA, saving a lot of time by not having to
                // reshuffle fields
                invert_quda_direct((double*) propagators_t0[dil_d], (double*) sources[dil_d], op_id);
              } else {
#endif
                // use standard inverter interface
                tmLQCD_invert((double *) propagators_t0[dil_d], (double *) sources[dil_d], op_id, write_prop);
              } // close either the if statement or the block (see #ifndef PG_QUDA_DIRECT)
              if(myid == 0)
                printf("inversion took %f seconds\n", omp_get_wtime() - t0_time);
            }
          }
          
          // this barrier is necessary to prevent concurrent access to propagators_t0 and propagators_t1
          #pragma omp barrier
          if( num_threads == 1 || thread_id == 0){
            // exchange pointers 
            propagators_temp = propagators_t1;
            propagators_t1 = propagators_t0;
            propagators_t0 = propagators_temp;
          }
          #pragma omp barrier
          
          // threads 1 to n-1 will enter add_to_perambulator while the master thread will move on to the next
          // iteration to generate the next set of sources and drive the inversion
          if( num_threads == 1 || thread_id >= 1 ){
            double t1_time = omp_get_wtime();
            dis.add_to_perambulator(dil_t,dil_e,propagators_t1);
            if( (num_threads == 1 || thread_id == 1) && myid == 0 )
              printf("add_to_perambulator took %f seconds\n", omp_get_wtime() - t1_time);
          }
        }
      } // end of loop over inversions
    } // OpenMP parallel section closing brace

    // free memory for sources and propagators to reduce memory load somewhat
    // before propagaror is written
    // these will be re-allocated above when the next random vector is about to be
    // processed
    for(size_t i = 0; i < nb_of_inversions; ++i){
      delete[] sources[i];
      delete[] propagators_t1[i];
      delete[] propagators_t0[i];
    }

    if( param.hack_clean ){
      // ----------- this is a total hack to use less memory such that the write buffer
      // allocated below to write the perambulator does not cause us to run out of
      // memory when running on a machine with very limited memory
      // this restricts us to doing only a single random vector per job, so
      // it should only be used as a last resort
      printf("HACK finalize tmLQCD\n");
      fflush(stdout);
      tmLQCD_finalise();
      sleep(5);
      MPI_Barrier(MPI_COMM_WORLD);
      printf("HACK clean distillery\n");
      fflush(stdout);
      dis.hack_clean();
      sleep(5);
      // ------------ this is a total hack to use less memory
    }

    MPI_Barrier(MPI_COMM_WORLD);
    // write perambulator to disc
    dis.write_perambulator_to_disk(rnd_id);
    MPI_Barrier(MPI_COMM_WORLD);
    // reset perambulator and create new random vector
    // memory reduction hack implies that this doesn't work anymore
    if(rnd_id < param.nb_rnd - 1 && !param.hack_clean)
      dis.reset_perambulator_and_randomvector(rnd_id+1);
    MPI_Barrier(MPI_COMM_WORLD);
  } // end of loop over random vectors

  dis.clean();

  MPI_Finalize();
	return 0;
}

