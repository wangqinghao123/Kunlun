#ifndef KUNLUN_IKNP_OTE_HPP_
#define KUNLUN_IKNP_OTE_HPP_

#include "naor_pinkas_ot.hpp"
/*
 * IKNP OT Extension
 * [REF] Implementation of "Extending oblivious transfers efficiently"
 * https://www.iacr.org/archive/crypto2003/27290145/27290145.pdf
 *
 * [REF] With optimization of "More Efficient Oblivious Transfer and Extensions for Faster Secure Computation"
 * https://eprint.iacr.org/2013/552.pdf
 * [REF] With optimization of "Better Concrete Security for Half-Gates Garbling (in the Multi-Instance Setting)"
 * https://eprint.iacr.org/2019/1168.pdf
 */

const static size_t BASE_LEN = 128; // the default length of base OT

namespace IKNPOTE{

using Serialization::operator<<; 
using Serialization::operator>>; 

// check if the parameters are legal
void CheckParameters(size_t ROW_NUM, size_t COLUMN_NUM)
{
    if (ROW_NUM%128 != 0 || COLUMN_NUM%128 != 0){
        std::cerr << "row or colulumn parameters is wrong" << std::endl;
        exit(EXIT_FAILURE); 
    }
}

struct PP
{
    uint8_t malicious = 0; // false
    NPOT::PP baseOT;   
};

void PrintPP(const PP &pp)
{
    std::cout << "malicious = " << int(pp.malicious) << std::endl; 
    NPOT::PrintPP(pp.baseOT);
}


// serialize pp to stream
std::ofstream &operator<<(std::ofstream &fout, const PP &pp)
{
	fout << pp.baseOT; 
    fout << pp.malicious; 
    return fout;
}


// deserialize pp from stream
std::ifstream &operator>>(std::ifstream &fin, PP &pp)
{
	fin >> pp.baseOT; 
    fin >> pp.malicious; 
    return fin; 
}

PP Setup()
{
    PP pp; 
    pp.malicious = 0; 
    pp.baseOT = NPOT::Setup();  
    return pp;
}

// save pp to file
void SavePP(PP &pp, std::string pp_filename)
{
	std::ofstream fout; 
    fout.open(pp_filename, std::ios::binary); 
    if(!fout)
    {
        std::cerr << pp_filename << " open error" << std::endl;
        exit(1); 
    }
    fout << pp; 
    fout.close(); 
}


// fetch pp from file
void FetchPP(PP &pp, std::string pp_filename)
{
	std::ifstream fin; 
    fin.open(pp_filename, std::ios::binary); 
    if(!fin)
    {
        std::cerr << pp_filename << " open error" << std::endl;
        exit(1); 
    }
    fin >> pp; 
    fin.close(); 
}

void Send(NetIO &io, PP &pp, std::vector<block> &vec_m0, std::vector<block> &vec_m1, size_t EXTEND_LEN) 
{
    /* 
    ** Phase 1: sender obtains a random secret sharing matrix Q of matrix T from receiver
    ** T is a tall matrix, to use base OT oblivious transfer T, 
    ** the sender first oblivous get 1-out-of-2 keys per column from receiver via base OT 
    ** receiver then send encryptions of the original column and shared column under k0 and k1 respectively
    */
    PrintSplitLine('-'); 
	auto start_time = std::chrono::steady_clock::now(); 

    // prepare to receive a secret shared matrix Q from receiver
    size_t ROW_NUM = EXTEND_LEN;   // set row num as the length of long ot
    size_t COLUMN_NUM = BASE_LEN;  // set column num as the length of base ot

    CheckParameters(ROW_NUM, COLUMN_NUM); 

    PRG::Seed seed = PRG::SetSeed(nullptr, 0); // initialize PRG seed

    // generate Phase 1 selection bit vector
    std::vector<uint8_t> vec_selection_bit(BASE_LEN);
    vec_selection_bit = GenRandomBits(seed, BASE_LEN); 

    // first receive 1-out-2 two keys from the receiver 
    std::vector<block> vec_K = NPOT::Receive(io, pp.baseOT, vec_selection_bit, BASE_LEN);

    std::cout << "IKNP OTE [step 1]: Sender obliviuosly get " << BASE_LEN 
              << " number of keys from Receiver via base OT" << std::endl; 
    /* 
    ** invoke base OT BASE_LEN times to obtain a matrix Q
    ** after receiving the key, begin to receive ciphertexts
    */

    std::vector<block> vec_inner_C0(ROW_NUM/128); // 1 block = 128 bits 
    std::vector<block> vec_inner_C1(ROW_NUM/128); 

    std::vector<block> vec_pad(ROW_NUM/128); // the one-time pad used to decrypt C
    std::vector<block> vec_plaintext(ROW_NUM/128);   // the plaintext
    
    std::vector<uint8_t> Q(ROW_NUM/8 * COLUMN_NUM); // the matrix sender is going to receive from receiver (dense form)
    // for every column: prepare two column vectors
    
    for(auto j = 0; j < BASE_LEN; j++){
        // receiver the two ciphertexts
        io.ReceiveBlocks(vec_inner_C0.data(), ROW_NUM/128); 
        io.ReceiveBlocks(vec_inner_C1.data(), ROW_NUM/128);

        // use K[i] as seed to derive the one-time pad
        PRG::ReSeed(seed, &vec_K[j], 0);    
        vec_pad = PRG::GenRandomBlocks(seed, ROW_NUM/128);

        if(vec_selection_bit[j] == 0){
            vec_plaintext = Block::XOR(vec_inner_C0, vec_pad);
        }
        else{
            vec_plaintext = Block::XOR(vec_inner_C1, vec_pad);   
        } 
        Block::ToDenseBits(vec_plaintext.data(), ROW_NUM/128, Q.data()+j*ROW_NUM/8, ROW_NUM); 
    }   

    #ifdef DEBUG
        std::cout << "IKNP OTE: Sender obliviuosly get "<< BASE_LEN << " pair of ciphertexts from Receiver" << std::endl; 
    #endif
    

    // transpose Q
    std::vector<uint8_t> Q_tanspose(ROW_NUM/8 * COLUMN_NUM); 
    empBitMatrixTranspose(Q.data(), ROW_NUM, COLUMN_NUM, Q_tanspose.data());  

    #ifdef DEBUG
        std::cout << "IKNP OTE: Sender transposes matrix Q" << std::endl; 
    #endif

    // generate dense representation of selection block
    std::vector<block> vec_selection_block(BASE_LEN/128); 
    Block::FromSparseBits(vec_selection_bit.data(), BASE_LEN, vec_selection_block.data(), BASE_LEN/128); 


    // begin to transmit the real message
    std::vector<block> vec_outer_C0(ROW_NUM); 
    std::vector<block> vec_outer_C1(ROW_NUM); 

    for(auto i = 0; i < ROW_NUM; i++)
    {
        std::vector<block> Q_row_block(BASE_LEN/128);

        Block::FromDenseBits(Q_tanspose.data() + i*COLUMN_NUM/8, BASE_LEN, Q_row_block.data(), BASE_LEN/128); 
        
        vec_outer_C0[i] = vec_m0[i]^Hash::BlocksToBlock(Q_row_block); 
        vec_outer_C1[i] = vec_m1[i]^Hash::BlocksToBlock(Block::XOR(Q_row_block, vec_selection_block));
    }
    io.SendBlocks(vec_outer_C0.data(), ROW_NUM); 
    io.SendBlocks(vec_outer_C1.data(), ROW_NUM);

    
    std::cout << "IKNP OTE [step 3]: Sender ===> (vec_C0, vec_C1) ===> Receiver" 
              << "[" << (double)ROW_NUM*16*2/(1024*1024) << " MB]" << std::endl; 

    auto end_time = std::chrono::steady_clock::now(); 
    auto running_time = end_time - start_time;
    std::cout << "IKNP OTE: Sender side takes time " 
              << std::chrono::duration <double, std::milli> (running_time).count() << " ms" << std::endl;
    PrintSplitLine('-'); 
}

std::vector<block> Receive(NetIO &io, PP &pp, const std::vector<uint8_t> &vec_selection_bit, size_t EXTEND_LEN)
{
    PrintSplitLine('-'); 
    std::vector<block> vec_result(EXTEND_LEN);
    // first act as sender in base OT
    auto start_time = std::chrono::steady_clock::now(); 

    // prepare a random matrix
    size_t ROW_NUM = EXTEND_LEN; 
    size_t COLUMN_NUM = BASE_LEN; 

    CheckParameters(ROW_NUM, COLUMN_NUM); 

    PRG::Seed seed = PRG::SetSeed(nullptr, 0); 
    
    std::vector<uint8_t> T = PRG::GenRandomBitMatrix(seed, ROW_NUM, COLUMN_NUM); 

    std::vector<block> vec_K0 = PRG::GenRandomBlocks(seed, BASE_LEN);
    std::vector<block> vec_K1 = PRG::GenRandomBlocks(seed, BASE_LEN);

    // Phase 1: first transmit 1-out-2 key to sender
    // NPOT::PP pp_npot; 
    // GetNPOTPP(pp, pp_npot); 
    NPOT::Send(io, pp.baseOT, vec_K0, vec_K1, BASE_LEN); 

    std::cout << "IKNP OTE [step 1]: Receiver transmits "<< BASE_LEN << " number of keys to Sender via base OT" 
              << std::endl; 

    // generate the dense representation of selection block
    std::vector<block> vec_selection_block(ROW_NUM/128); 
    Block::FromSparseBits(vec_selection_bit.data(), ROW_NUM, vec_selection_block.data(), ROW_NUM/128); 


    // Phase 1: transmit ciphertext a.k.a. random shared matrix
    std::vector<block> vec_m0(ROW_NUM/128); 
    std::vector<block> vec_m1(ROW_NUM/128); 

    std::vector<block> vec_inner_C0(ROW_NUM/128); 
    std::vector<block> vec_inner_C1(ROW_NUM/128); 
    
    std::vector<block> vec_pad(ROW_NUM/128);
    // for every column: prepare two column vectors
    for(auto j = 0; j < COLUMN_NUM; j++)
    {
        // set vec_m0 be the jth column of T
        Block::FromDenseBits(T.data() + j*ROW_NUM/8, ROW_NUM, vec_m0.data(), ROW_NUM/128); 

        // set vec_m1 = vec_m0 xor selection_block
        vec_m1 = Block::XOR(vec_m0, vec_selection_block);

        PRG::ReSeed(seed, &vec_K0[j], 0); 
        vec_pad = PRG::GenRandomBlocks(seed, ROW_NUM/128);
        vec_inner_C0 = Block::XOR(vec_m0, vec_pad); 
        
        PRG::ReSeed(seed, &vec_K1[j], 0); 
        vec_pad = PRG::GenRandomBlocks(seed, ROW_NUM/128);
        vec_inner_C1 = Block::XOR(vec_m1, vec_pad);

        io.SendBlocks(vec_inner_C0.data(), ROW_NUM/128); 
        io.SendBlocks(vec_inner_C1.data(), ROW_NUM/128);
    }   

    std::cout << "IKNP OTE [step 2]: Receiver ===> 2 encrypted matrix ===> Sender" 
              << " [" << (double)COLUMN_NUM*ROW_NUM/128*16*2/(1024*1024) << " MB]" << std::endl; 
    
    std::vector<uint8_t> T_transpose(ROW_NUM/8 * COLUMN_NUM); 
    empBitMatrixTranspose(T.data(), ROW_NUM, COLUMN_NUM, T_transpose.data());

    #ifdef DEBUG
        std::cout << "IKNP OTE: Receiver transposes matrix T" << std::endl; 
    #endif

    std::vector<block> vec_outer_C0(ROW_NUM); 
    std::vector<block> vec_outer_C1(ROW_NUM); 

    io.ReceiveBlocks(vec_outer_C0.data(), ROW_NUM);
    io.ReceiveBlocks(vec_outer_C1.data(), ROW_NUM);


    #ifdef DEBUG
        std::cout << "IKNP OTE: Receiver get "<< ROW_NUM << " pair of ciphertexts from Sender" << std::endl; 
    #endif

    for(auto i = 0; i < ROW_NUM; i++)
    {
        std::vector<block> T_row_block(BASE_LEN/128);  
        Block::FromDenseBits(T_transpose.data()+i*COLUMN_NUM/8, BASE_LEN, T_row_block.data(), BASE_LEN/128); 
        
        if(vec_selection_bit[i] == 0){
            vec_result[i] = vec_outer_C0[i]^Hash::BlocksToBlock(T_row_block); 
        }
        else{
            vec_result[i] = vec_outer_C1[i]^Hash::BlocksToBlock(T_row_block);
        }
    }   

    #ifdef DEBUG
        std::cout << "IKNP OTE: Receiver obtains "<< ROW_NUM << " number of messages from Sender" << std::endl; 
        PrintSplitLine('*'); 
    #endif

    std::cout << "IKNP OTE [step 4]: Receiver obtains vec_m" << std::endl; 

    auto end_time = std::chrono::steady_clock::now(); 
    auto running_time = end_time - start_time;
    std::cout << "IKNP OTE: Receiver side takes time " 
              << std::chrono::duration <double, std::milli> (running_time).count() << " ms" << std::endl;

    PrintSplitLine('-'); 

    return vec_result; 
}

void OnesidedSend(NetIO &io, PP &pp, std::vector<block> &vec_m, size_t EXTEND_LEN) 
{
    /* 
    ** Phase 1: sender obtains a random secret sharing matrix Q of matrix T from receiver
    ** T is a tall matrix, to use base OT oblivious transfer T, 
    ** the sender first oblivous get 1-out-of-2 keys per column from receiver via base OT 
    ** receiver then send encryptions of the original column and shared column under k0 and k1 respectively
    */	
    PrintSplitLine('-'); 
	
    auto start_time = std::chrono::steady_clock::now(); 

    // prepare to receive a secret shared matrix Q from receiver
    size_t ROW_NUM = EXTEND_LEN;   // set row num as the length of long ot
    size_t COLUMN_NUM = BASE_LEN;  // set column num as the length of base ot

    CheckParameters(ROW_NUM, COLUMN_NUM); 

    PRG::Seed seed = PRG::SetSeed(nullptr, 0); // initialize PRG seed

    // generate Phase 1 selection bit vector
    std::vector<uint8_t> vec_selection_bit(BASE_LEN);
    vec_selection_bit = GenRandomBits(seed, BASE_LEN); 

    // first receive 1-out-2 two keys from the receiver 
    std::vector<block> vec_K = NPOT::Receive(io, pp.baseOT, vec_selection_bit, BASE_LEN);

    std::cout << "IKNP OTE [step 1]: Sender obliviuosly get "
              << BASE_LEN << " number of keys from Receiver via base OT" << std::endl; 
    /* 
    ** invoke base OT BASE_LEN times to obtain a matrix Q
    ** after receiving the key, begin to receive ciphertexts
    */

    std::vector<block> vec_inner_C0(ROW_NUM/128); // 1 block = 128 bits 
    std::vector<block> vec_inner_C1(ROW_NUM/128); 

    std::vector<block> vec_pad(ROW_NUM/128); // the one-time pad used to decrypt C
    std::vector<block> vec_plaintext(ROW_NUM/128);   // the plaintext
    
    std::vector<uint8_t> Q(ROW_NUM/8 * COLUMN_NUM); // the matrix sender is going to receive from receiver (dense form)
    // for every column: prepare two column vectors
    for(auto j = 0; j < BASE_LEN; j++){
        // receiver the two ciphertexts
        io.ReceiveBlocks(vec_inner_C0.data(), ROW_NUM/128); 
        io.ReceiveBlocks(vec_inner_C1.data(), ROW_NUM/128);

        // use K[i] as seed to derive the one-time pad
        PRG::ReSeed(seed, &vec_K[j], 0);    
        vec_pad = PRG::GenRandomBlocks(seed, ROW_NUM/128);

        if(vec_selection_bit[j] == 0){
            vec_plaintext = Block::XOR(vec_inner_C0, vec_pad);
        }
        else{
            vec_plaintext = Block::XOR(vec_inner_C1, vec_pad);   
        } 
        Block::ToDenseBits(vec_plaintext.data(), ROW_NUM/128, Q.data()+j*ROW_NUM/8, ROW_NUM); 
    }   

    #ifdef DEBUG
        std::cout << "IKNP OTE: Sender obliviuosly get "<< BASE_LEN << " pair of ciphertexts from Receiver" << std::endl; 
    #endif
    

    // transpose Q
    std::vector<uint8_t> Q_tanspose(ROW_NUM/8 * COLUMN_NUM); 
    empBitMatrixTranspose(Q.data(), ROW_NUM, COLUMN_NUM, Q_tanspose.data());  

    #ifdef DEBUG
        std::cout << "IKNP OTE: Sender transposes matrix Q" << std::endl; 
    #endif

    // generate dense representation of selection block
    std::vector<block> vec_selection_block(BASE_LEN/128); 
    Block::FromSparseBits(vec_selection_bit.data(), BASE_LEN, vec_selection_block.data(), BASE_LEN/128); 

    // begin to transmit the real message
    block outer_C; 

    for(auto i = 0; i < ROW_NUM; i++)
    {
        std::vector<block> Q_row_block(BASE_LEN/128);
        Block::FromDenseBits(Q_tanspose.data() + i*COLUMN_NUM/8, BASE_LEN, Q_row_block.data(), BASE_LEN/128);
        outer_C = vec_m[i]^Hash::BlocksToBlock(Block::XOR(Q_row_block, vec_selection_block));
        io.SendBlock(outer_C); 
    }

    std::cout << "IKNP OTE [step 3]: Sender ===> vec_C ===> Receiver" << " [" 
              << (double)ROW_NUM*16/(1024*1024) << " MB]" << std::endl;

    #ifdef DEBUG
        std::cout << "IKNP OTE: Sender sends "<< ROW_NUM << " number of ciphertexts to receiver" << std::endl; 
        PrintSplitLine('*'); 
    #endif

    auto end_time = std::chrono::steady_clock::now(); 
    auto running_time = end_time - start_time;
    std::cout << "IKNP OTE: Sender side takes time " 
              << std::chrono::duration <double, std::milli> (running_time).count() << " ms" << std::endl;

    PrintSplitLine('-'); 
}

// the size of vec_result = the hamming weight of vec_selection_bit
std::vector<block> OnesidedReceive(NetIO &io, PP &pp, const std::vector<uint8_t> &vec_selection_bit, size_t EXTEND_LEN)
{
    PrintSplitLine('-'); 

    std::vector<block> vec_result;
    // first act as sender in base OT
    
    auto start_time = std::chrono::steady_clock::now(); 
    // prepare a random matrix
    size_t ROW_NUM = EXTEND_LEN; 
    size_t COLUMN_NUM = BASE_LEN; 

    CheckParameters(ROW_NUM, COLUMN_NUM); 

    PRG::Seed seed = PRG::SetSeed(nullptr, 0); 
    
    std::vector<uint8_t> T = PRG::GenRandomBitMatrix(seed, ROW_NUM, COLUMN_NUM); 

    std::vector<block> vec_K0 = PRG::GenRandomBlocks(seed, BASE_LEN);
    std::vector<block> vec_K1 = PRG::GenRandomBlocks(seed, BASE_LEN);

    // Phase 1: first transmit 1-out-2 key to sender
    NPOT::Send(io, pp.baseOT, vec_K0, vec_K1, BASE_LEN); 

    std::cout << "IKNP OTE [step 1]: Receiver sends "<< BASE_LEN << " number of keys to Sender via base OT" << std::endl; 

    // generate the dense representation of selection block
    std::vector<block> vec_selection_block(ROW_NUM/128); 
    Block::FromSparseBits(vec_selection_bit.data(), ROW_NUM, vec_selection_block.data(), ROW_NUM/128); 


    // Phase 1: transmit ciphertext a.k.a. random shared matrix
    std::vector<block> vec_m0(ROW_NUM/128); 
    std::vector<block> vec_m1(ROW_NUM/128); 

    std::vector<block> vec_inner_C0(ROW_NUM/128); 
    std::vector<block> vec_inner_C1(ROW_NUM/128); 
    
    std::vector<block> vec_pad(ROW_NUM/128);
    // for every column: prepare two column vectors
    for(auto j = 0; j < COLUMN_NUM; j++)
    {
        // set vec_m0 be the jth column of T
        Block::FromDenseBits(T.data() + j*ROW_NUM/8, ROW_NUM, vec_m0.data(), ROW_NUM/128); 

        // set vec_m1 = vec_m0 xor selection_block
        vec_m1 = Block::XOR(vec_m0, vec_selection_block);

        PRG::ReSeed(seed, &vec_K0[j], 0); 
        vec_pad = PRG::GenRandomBlocks(seed, ROW_NUM/128);
        vec_inner_C0 = Block::XOR(vec_m0, vec_pad); 
        
        PRG::ReSeed(seed, &vec_K1[j], 0); 
        vec_pad = PRG::GenRandomBlocks(seed, ROW_NUM/128);
        vec_inner_C1 = Block::XOR(vec_m1, vec_pad);

        io.SendBlocks(vec_inner_C0.data(), ROW_NUM/128); 
        io.SendBlocks(vec_inner_C1.data(), ROW_NUM/128);
    }   
    
    std::cout << "IKNP OTE [step 2]: Receiver ===> 2 encrypted matrix ===> Sender" 
              << " [" << (double)COLUMN_NUM*ROW_NUM/128*16*2/(1024*1024) << " MB]" << std::endl; 

    std::vector<uint8_t> T_transpose(ROW_NUM/8 * COLUMN_NUM); 
    empBitMatrixTranspose(T.data(), ROW_NUM, COLUMN_NUM, T_transpose.data());

    #ifdef DEBUG
        std::cout << "IKNP OTE: Receiver transposes matrix T" << std::endl; 
    #endif

    block outer_C; 
    
    for(auto i = 0; i < ROW_NUM; i++)
    {
        io.ReceiveBlock(outer_C);
        std::vector<block> T_row_block(BASE_LEN/128);  
        Block::FromDenseBits(T_transpose.data()+i*COLUMN_NUM/8, BASE_LEN, T_row_block.data(), BASE_LEN/128); 
        
        // only decrypt when selection bit is 1
        if(vec_selection_bit[i] == 1){
            vec_result.emplace_back(outer_C^Hash::BlocksToBlock(T_row_block));
        }
    }   

    #ifdef DEBUG
        std::cout << "IKNP OTE: Receiver get "<< ROW_NUM << " number of ciphertexts from Sender" << std::endl; 
        PrintSplitLine('*'); 
    #endif

    std::cout << "IKNP OTE [step 4]: Receiver obtains vec_m" << std::endl; 

    auto end_time = std::chrono::steady_clock::now(); 
    auto running_time = end_time - start_time;
    std::cout << "IKNP OTE: Receiver side takes time " 
              << std::chrono::duration <double, std::milli> (running_time).count() << " ms" << std::endl;

    PrintSplitLine('-'); 

    return vec_result; 
}

}
#endif