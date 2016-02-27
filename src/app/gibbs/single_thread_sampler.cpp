#include "app/gibbs/single_thread_sampler.h"

namespace dd {

SingleThreadSampler::SingleThreadSampler(FactorGraph *_p_fg,
                                         bool sample_evidence, bool burn_in,
                                         bool learn_non_evidence)
    : p_fg(_p_fg),
      p_rand_obj_buf(new double),
      sample_evidence(sample_evidence),
      burn_in(burn_in),
      learn_non_evidence(learn_non_evidence) {
  p_rand_seed[0] = rand();
  p_rand_seed[1] = rand();
  p_rand_seed[2] = rand();
}

void SingleThreadSampler::sample(const int &i_sharding, const int &n_sharding) {
  long nvar = p_fg->n_var;
  // calculates the start and end id in this partition
  long start = ((long)(nvar / n_sharding) + 1) * i_sharding;
  long end = ((long)(nvar / n_sharding) + 1) * (i_sharding + 1);
  end = end > nvar ? nvar : end;

  // sample each variable in the partition
  for (long i = start; i < end; i++) {
    this->sample_single_variable(i, p_fg->is_inc);
  }
}

void SingleThreadSampler::sample_sgd(const int &i_sharding,
                                     const int &n_sharding) {
  long nvar = p_fg->n_var;
  long start = ((long)(nvar / n_sharding) + 1) * i_sharding;
  long end = ((long)(nvar / n_sharding) + 1) * (i_sharding + 1);
  end = end > nvar ? nvar : end;
  for (long i = start; i < end; i++) {
    this->sample_sgd_single_variable(i);
  }
}

void SingleThreadSampler::sample_sgd_single_variable(long vid) {
  // stochastic gradient ascent
  // gradient of weight = E[f|D] - E[f], where D is evidence variables,
  // f is the factor function, E[] is expectation. Expectation is calculated
  // using a sample of the variable.

  Variable &variable = p_fg->variables[vid];
  if (variable.is_observation) return;

  if (!learn_non_evidence && !variable.is_evid) return;

  int proposal = 0;

  // sample the variable with evidence unchanged
  if (variable.is_evid == false) {
    proposal = draw_sample(variable, false);
    p_fg->update_evid(variable, (double)proposal);
  }

  // sample the variable regardless of whether it's evidence
  proposal = draw_sample(variable, true);
  p_fg->template update<true>(variable, (double)proposal);

  this->p_fg->update_weight(variable);
}

void SingleThreadSampler::sample_single_variable(long vid, bool is_inc) {
  // this function uses the same sampling technique as in
  // sample_sgd_single_variable

  Variable &variable = this->p_fg->variables[vid];
  if (variable.is_observation) return;

  if (is_inc) {
    if (variable.domain_type == DTYPE_BOOLEAN ||
        variable.domain_type == DTYPE_MULTINOMIAL) {
      if (variable.is_evid == false || sample_evidence) {
        int newvalue = variable.next_sample;
        // std::cout << newvalue << std::endl;
        int oldvalue = p_fg->infrs->assignments_evid[vid];

        // if(newvalue == oldvalue){
        // p_fg->template update<false>(variable, newvalue);
        // std::cout << "/" << std::endl;
        // accept
        //}else{

        potential_pos =
            p_fg->template potential<false>(variable, newvalue);  // flip
        potential_neg =
            p_fg->template potential<false>(variable, oldvalue);  // not flip
        // std::cout << vid << " -> " << newvalue << "(" << potential_pos << ")"
        //      << "    " << oldvalue << "(" << potential_neg << ")" <<
        //      std::endl;
        *this->p_rand_obj_buf = erand48(this->p_rand_seed);
        float r = (*this->p_rand_obj_buf);
        float prob = exp(potential_pos - potential_neg);
        // std::cout << r << "  " << prob << std::endl;

        if (r < prob) {
          p_fg->template update<false>(variable, newvalue);
        } else {
          p_fg->template update<false>(variable, oldvalue);
          // std::cout << "~" << std::endl;
        }
        //}
      }

    } else {
      std::cout << "INC DOES NOT SUPPORT BOOLEAN" << std::endl;
    }
  } else {
    if (variable.is_evid == false || sample_evidence) {
      int proposal = draw_sample(variable, sample_evidence);
      p_fg->template update<false>(variable, (double)proposal);
    }
  }
}
}

int dd::SingleThreadSampler::draw_sample(Variable &variable,
                                         bool is_free_sample) {
  if (variable.is_evid && !is_free_sample) return variable.assignment_evid;

  int proposal = 0;

  switch (variable.domain_type) {
    case DTYPE_BOOLEAN:
      if (is_free_sample) {
        potential_pos = p_fg->template potential<true>(variable, 1);
        potential_neg = p_fg->template potential<true>(variable, 0);
      } else {
        potential_pos = p_fg->template potential<false>(variable, 1);
        potential_neg = p_fg->template potential<false>(variable, 0);
      }

      *this->p_rand_obj_buf = erand48(this->p_rand_seed);
      // sample the variable
      // flip a coin with probability
      // (exp(potential_pos) + exp(potential_neg)) / exp(potential_neg)
      // = exp(potential_pos - potential_neg) + 1
      if ((*this->p_rand_obj_buf) * (1.0 + exp(potential_neg - potential_pos)) <
          1.0) {
        proposal = 1;
      } else {
        proposal = 0;
      }
      break;

    case DTYPE_MULTINOMIAL:
      // varlen_potential_buffer contains potential for each proposals
      while (variable.upper_bound >= varlen_potential_buffer.size()) {
        varlen_potential_buffer.push_back(0.0);
      }

      sum = -100000.0;
      acc = 0.0;
      proposal = -1;

      // calculate potential for each proposal
      for (int propose = variable.lower_bound; propose <= variable.upper_bound;
           propose++) {
        if (is_free_sample) {
          varlen_potential_buffer[propose] =
              p_fg->template potential<true>(variable, propose);
        } else {
          varlen_potential_buffer[propose] =
              p_fg->template potential<false>(variable, propose);
        }
        sum = logadd(sum, varlen_potential_buffer[propose]);
      }

      // flip a coin
      *this->p_rand_obj_buf = erand48(this->p_rand_seed);
      for (int propose = variable.lower_bound; propose <= variable.upper_bound;
           propose++) {
        acc += exp(varlen_potential_buffer[propose] - sum);
        if (*this->p_rand_obj_buf <= acc) {
          proposal = propose;
          break;
        }
      }

      assert(proposal != -1);
      break;

    default:
      std::cerr
          << "[ERROR] Only Boolean and Multinomial variables are supported now!"
          << std::endl;
      abort();
  }

  return proposal;
}
