// Created by Saliya Ekanayake on 1/6/19.

#ifndef LBL_DAL_PARALLEL_FASTA_READER_HPP
#define LBL_DAL_PARALLEL_FASTA_READER_HPP


#include "ParallelOps.hpp"
#include "FastaData.hpp"

class ParallelFastaReader {
public:
  void readFasta(const char *file, ushort overlap, ushort k,
                 int rank, int world_size, std::shared_ptr<FastaData> &fd);
};


#endif //LBL_DAL_PARALLEL_FASTA_READER_HPP
