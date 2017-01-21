#pragma once

#include "data/corpus.h"
#include "graph/expression_graph.h"
#include "layers/rnn.h"
#include "layers/param_initializers.h"
#include "3rd_party/cnpy/cnpy.h"

namespace marian {

class Nematus : public ExpressionGraph {
  public:
    Nematus()
    : dimSrcVoc_(40000), dimSrcEmb_(512), dimEncState_(1024),
      dimTrgVoc_(40000), dimTrgEmb_(512), dimDecState_(1024),
      dimBatch_(40) {}

    Nematus(const std::vector<int> dims)
    : dimSrcVoc_(dims[0]), dimSrcEmb_(dims[1]), dimEncState_(dims[2]),
      dimTrgVoc_(dims[3]), dimTrgEmb_(dims[4]), dimDecState_(dims[5]),
      dimBatch_(dims[6]) {}

  private:
    int dimSrcVoc_;
    int dimSrcEmb_;
    int dimEncState_;

    int dimTrgVoc_;
    int dimTrgEmb_;
    int dimDecState_;

    int dimBatch_;

    void setDims(const data::CorpusBatch& batch) {
      dimSrcVoc_ = this->get("Wemb") ? this->get("Wemb")->shape()[0] : dimSrcVoc_;
      dimSrcEmb_ = this->get("Wemb") ? this->get("Wemb")->shape()[1] : dimSrcEmb_;
      dimEncState_ = this->get("encoder_U") ? this->get("encoder_U")->shape()[0] : dimEncState_;

      dimTrgVoc_ = this->get("Wemb_dec") ? this->get("Wemb_dec")->shape()[0] : dimTrgVoc_;
      dimTrgEmb_ = this->get("Wemb_dec") ? this->get("Wemb_dec")->shape()[1] : dimTrgEmb_;
      dimDecState_ = this->get("decoder_U") ? this->get("decoder_U")->shape()[0] : dimDecState_;

      dimBatch_ = batch.size();
    }

    RNN<GRUFast> encoderGRU(const std::string& prefix) {
      using namespace keywords;

      auto U = this->param(prefix + "_U", {dimEncState_, 2 * dimEncState_},
                           init=inits::glorot_uniform);

      auto W = this->param(prefix + "_W", {dimSrcEmb_, 2 * dimEncState_},
                           init=inits::glorot_uniform);

      auto b = this->param(prefix + "_b", {1, 2 * dimEncState_},
                           init=inits::zeros);

      auto Ux = this->param(prefix + "_Ux", {dimEncState_, dimEncState_},
                            init=inits::glorot_uniform);

      auto Wx = this->param(prefix + "_Wx", {dimSrcEmb_, dimEncState_},
                            init=inits::glorot_uniform);

      auto bx = this->param(prefix + "_bx", {1, dimEncState_},
                            init=inits::zeros);

      ParametersGRUFast encParams;
      encParams.U = concatenate({U, Ux}, axis=1);
      encParams.W = concatenate({W, Wx}, axis=1);
      encParams.b = concatenate({b, bx}, axis=1);

      //auto dropoutConstant = this->ones(shape={dimBatch_, dimEncState_});
      //auto dropoutMask = dropout(dropoutConstant, value=0.2);

      GRUFast gru(encParams /*, dropoutMask*/);
      return RNN<GRUFast>(gru);
    };

    RNN<GRUWithAttention> decoderGRUWithAttention() {
      using namespace keywords;

      ParametersGRUWithAttention decParams;

      auto U = this->param("decoder_U", {dimDecState_, 2 * dimDecState_},
                        init=inits::glorot_uniform);

      auto W = this->param("decoder_W", {dimTrgEmb_, 2 * dimDecState_},
                        init=inits::glorot_uniform);

      auto b = this->param("decoder_b", {1, 2 * dimDecState_},
                        init=inits::zeros);

      auto Ux = this->param("decoder_Ux", {dimDecState_, dimDecState_},
                        init=inits::glorot_uniform);

      auto Wx = this->param("decoder_Wx", {dimTrgEmb_, dimDecState_},
                         init=inits::glorot_uniform);

      auto bx = this->param("decoder_bx", {1, dimDecState_},
                         init=inits::zeros);

      decParams.U = concatenate({U, Ux}, axis=1);
      decParams.W = concatenate({W, Wx}, axis=1);
      decParams.b = concatenate({b, bx}, axis=1);

      decParams.Wa = this->param("decoder_W_comb_att", {dimDecState_, 2 * dimDecState_},
                                 init=inits::glorot_uniform);

      decParams.ba = this->param("decoder_b_att", {1, 2 * dimDecState_},
                                 init=inits::zeros);

      decParams.Ua = this->param("decoder_Wc_att", {2 * dimEncState_, 2 * dimDecState_},
                                 init=inits::glorot_uniform);

      decParams.va = this->param("decoder_U_att", {2 * dimDecState_, 1},
                                 init=inits::glorot_uniform);

      auto Uc = this->param("decoder_U_nl", {dimDecState_, 2 * dimDecState_},
                            init=inits::glorot_uniform);

      auto Wc = this->param("decoder_Wc", {2 * dimEncState_, 2 * dimDecState_},
                            init=inits::glorot_uniform);

      auto bc = this->param("decoder_b_nl", {1, 2 * dimDecState_},
                            init=inits::zeros);

      auto Uxc = this->param("decoder_Ux_nl", {dimDecState_, dimDecState_},
                             init=inits::glorot_uniform);

      auto Wxc = this->param("decoder_Wcx", {2 * dimEncState_, dimDecState_},
                             init=inits::glorot_uniform);

      auto bxc = this->param("decoder_bx_nl", {1, dimDecState_},
                             init=inits::zeros);

      decParams.Uc = concatenate({Uc, Uxc}, axis=1);
      decParams.Wc = concatenate({Wc, Wxc}, axis=1);
      decParams.bc = concatenate({bc, bxc}, axis=1);

      auto encoderContext = this->get("encoderContext");
      auto encoderContextWeights = this->get("encoderContextWeights");

      //auto dropoutConstant = this->ones(shape={dimBatch_, dimDecState_});
      //auto dropoutMask = dropout(dropoutConstant, value=0.2);

      GRUWithAttention gruCell(decParams,
                               encoderContext,
                               encoderContextWeights);

      return RNN<GRUWithAttention>(gruCell);
    };

  public:

    void load(const std::string& name) {
      using namespace keywords;

      std::cerr << "Loading model from " << name << std::endl;
      auto numpy = cnpy::npz_load(name);

      auto parameters = {
        // Source word embeddings
        "Wemb",

        // GRU in encoder
        "encoder_U", "encoder_W", "encoder_b",
        "encoder_Ux", "encoder_Wx", "encoder_bx",

        // GRU in encoder, reversed
        "encoder_r_U", "encoder_r_W", "encoder_r_b",
        "encoder_r_Ux", "encoder_r_Wx", "encoder_r_bx",

        // Transformation of decoder input state
        "ff_state_W", "ff_state_b",

        // Target word embeddings
        "Wemb_dec",

        // GRU layer 1 in decoder
        "decoder_U", "decoder_W", "decoder_b",
        "decoder_Ux", "decoder_Wx", "decoder_bx",

        // Attention
        "decoder_W_comb_att", "decoder_b_att",
        "decoder_Wc_att", "decoder_U_att",

        // GRU layer 2 in decoder
        "decoder_U_nl", "decoder_Wc", "decoder_b_nl",
        "decoder_Ux_nl", "decoder_Wcx", "decoder_bx_nl",

        // Read out
        "ff_logit_lstm_W", "ff_logit_lstm_b",
        "ff_logit_prev_W", "ff_logit_prev_b",
        "ff_logit_ctx_W", "ff_logit_ctx_b",
        "ff_logit_W", "ff_logit_b",
      };

      for(auto name : parameters) {
        Shape shape;
        if(numpy[name].shape.size() == 2) {
          shape.set(0, numpy[name].shape[0]);
          shape.set(1, numpy[name].shape[1]);
        }
        else if(numpy[name].shape.size() == 1) {
          shape.set(0, 1);
          shape.set(1, numpy[name].shape[0]);
        }

        this->param(name, shape,
                    init=inits::from_numpy(numpy[name]));
      }
    }

    void save(const std::string& name) {
      std::cerr << "Saving to " << name << std::endl;
      unsigned shape[2];
      std::string mode = "w";
      for(auto p : this->params().getMap()) {
        //std::cerr << p.first << " "
        //  << p.second->shape()[0] << "x"
        //  << p.second->shape()[1] << std::endl;

        std::vector<float> v;
        p.second->val() >> v;

        unsigned dim;
        if(p.second->shape()[0] == 1) {
          shape[0] = p.second->shape()[1];
          dim = 1;
        }
        else {
          shape[0] = p.second->shape()[0];
          shape[1] = p.second->shape()[1];
          dim = 2;
        }

        cnpy::npz_save(name, p.first, v.data(), shape, dim, mode);
        mode = "a";
      }

      float ctt = 0;
      shape[0] = 1;
      cnpy::npz_save(name, "decoder_c_tt", &ctt, shape, 1, mode);
    }

    void constructEncoder(const data::SentBatch& srcSentenceBatch) {
      using namespace keywords;

      auto Wemb = this->param("Wemb", {dimSrcVoc_, dimSrcEmb_},
                              init=inits::glorot_uniform);

      std::vector<size_t> indeces;
      std::vector<float> weightMask;

      std::vector<Expr> inputs;
      std::vector<std::pair<Expr, Expr>> inputsWithMask;

      for(auto& srcWordBatch : srcSentenceBatch) {
        for(auto i: srcWordBatch.first)
          indeces.push_back(i);
        for(auto m: srcWordBatch.second)
          weightMask.push_back(m);
      }

      int srcWords = (int)srcSentenceBatch.size();
      auto x = reshape(rows(Wemb, indeces), {dimBatch_, dimSrcEmb_, srcWords});
      auto xMask = this->constant(shape={dimBatch_, 1, srcWords},
                                  init=inits::from_vector(weightMask));

      auto encState0 = name(this->zeros(shape={dimBatch_, dimEncState_}),
                            "start");

      auto statesFw = encoderGRU("encoder").apply(x, encState0);
      auto statesBw = encoderGRU("encoder_r").apply(x, encState0, xMask, true);

      std::vector<Expr> biStates;
      auto itFw = statesFw.begin();
      auto itBw = statesBw.rbegin();
      while(itFw != statesFw.end()) {
        biStates.push_back(concatenate({*itFw++, *itBw++}, axis=1));
      }

      auto encContext = name(concatenate(biStates, axis=2), "encoderContext");

      auto weights = this->constant(shape={dimBatch_, 1, (int)statesFw.size()},
                                    init=inits::from_vector(weightMask));
      name(weights, "encoderContextWeights");

      auto meanContext = name(weighted_average(encContext, weights, axis=2),
                              "meanContext");
    }

    void constructDecoder(const data::SentBatch& trgSentenceBatch) {
      using namespace keywords;

      // *** Map mean encoder state to decoder start state ***
      auto Wi = this->param("ff_state_W", {2 * dimEncState_, dimDecState_},
                            init=inits::glorot_uniform);
      auto bi = this->param("ff_state_b", {1, dimDecState_},
                            init=inits::zeros);

      auto meanContext = this->get("meanContext");
      auto decState0 = tanh(affine(meanContext, Wi, bi));

      // *** Collect target embeddings and target indices ***
      auto Wemb_dec = this->param("Wemb_dec", {dimTrgVoc_, dimTrgEmb_},
                                  init=inits::glorot_uniform);

      std::vector<Expr> outputs;
      auto emptyEmbedding = this->zeros(shape={dimBatch_, dimTrgEmb_});

      std::vector<float> weightMask;
      std::vector<float> picks;
      std::vector<size_t> indeces;
      for(int j = 0; j < trgSentenceBatch.size(); ++j) {
        auto& trgWordBatch = trgSentenceBatch[j];

        for(auto i : trgWordBatch.first) {
          picks.push_back((float)i);
          if(j < trgSentenceBatch.size() - 1)
            indeces.push_back(i);
        }

        for(auto m : trgWordBatch.second)
            weightMask.push_back(m);
      }

      auto picksTensor = this->constant(shape={(int)picks.size(), 1},
                                        init=inits::from_vector(picks));

      int trgWords = (int)trgSentenceBatch.size();
      auto y = reshape(rows(Wemb_dec, indeces), {dimBatch_, dimTrgEmb_, trgWords - 1});
      y = concatenate({emptyEmbedding, y}, axis=2);

      // *** Apply conditional GRU to target embeddings ***
      auto decoderGRU = decoderGRUWithAttention();
      auto decStates = decoderGRU.apply(y, decState0);

      auto contexts = decoderGRU.getCell().getContexts();

      // *** Final output layers ***
      auto d1 = concatenate(decStates, axis=2);
      auto c3 = concatenate(contexts, axis=2);

      auto W1 = this->param("ff_logit_lstm_W", {dimDecState_, dimTrgEmb_},
                            init=inits::glorot_uniform);
      auto b1 = this->param("ff_logit_lstm_b", {1, dimTrgEmb_},
                            init=inits::zeros);

      auto W2 = this->param("ff_logit_prev_W", {dimTrgEmb_, dimTrgEmb_},
                            init=inits::glorot_uniform);
      auto b2 = this->param("ff_logit_prev_b", {1, dimTrgEmb_},
                            init=inits::zeros);

      auto W3 = this->param("ff_logit_ctx_W", {2 * dimEncState_, dimTrgEmb_},
                            init=inits::glorot_uniform);
      auto b3 = this->param("ff_logit_ctx_b", {1, dimTrgEmb_},
                            init=inits::zeros);

      auto W4 = this->param("ff_logit_W", {dimTrgEmb_, dimTrgVoc_},
                            init=inits::glorot_uniform);
      auto b4 = this->param("ff_logit_b", {1, dimTrgVoc_},
                            init=inits::zeros);

      auto t = tanhPlus3(affine(d1, W1, b1),
                         affine(y, W2, b2),
                         affine(c3, W3, b3));

      auto aff = affine(t, W4, b4);

      auto weights = this->constant(shape={dimBatch_, 1, (int)decStates.size()},
                                    init=inits::from_vector(weightMask));

      // *** Cross entropy and cost across words and batch ***
      auto xe = cross_entropy(aff, picksTensor) * weights;
      auto cost = name(mean(sum(xe, axis=2), axis=0), "cost");
    }

    float cost() {
      return this->get("cost")->val()->scalar();
    }

    void construct(const data::CorpusBatch& batch) {
      this->clear();

      setDims(batch);
      constructEncoder(batch[0]);
      constructDecoder(batch[1]);
    }
};

}