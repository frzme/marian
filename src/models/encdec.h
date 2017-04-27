#pragma once

#include "data/corpus.h"
#include "data/batch_generator.h"

#include "training/config.h"
#include "graph/expression_graph.h"
#include "layers/param_initializers.h"
#include "layers/generic.h"
#include "common/logging.h"
#include "models/states.h"
#include "models/lex_probs.h"

namespace marian {

class EncoderBase {
  protected:
    Ptr<Config> options_;
    std::string prefix_{"encoder"};
    bool inference_{false};

    virtual std::tuple<Expr, Expr>
    prepareSource(Expr srcEmbeddings,
                  Expr posEmbeddings,
                  Ptr<data::CorpusBatch> batch,
                  size_t index) {
      using namespace keywords;
      
      auto subBatch = (*batch)[index];
      
      int dimBatch = subBatch->batchSize();
      int dimEmb = srcEmbeddings->shape()[1];
      int dimWords = subBatch->batchWidth();

      auto graph = srcEmbeddings->graph();
      auto chosenEmbeddings = rows(srcEmbeddings, subBatch->indeces());
      
      if(posEmbeddings) {
        int dimPos = options_->get<int>("dim-pos");
        int dimMax = options_->get<size_t>("max-length");
        
        std::vector<size_t> positions(subBatch->indeces().size());
        for(int i = 0; i < positions.size(); ++i) {
          int pos = i / dimBatch;
          positions[i] = pos < dimMax ? pos : dimMax;
        }
        
        auto chosenPositions = rows(posEmbeddings, positions);
        chosenEmbeddings = concatenate({chosenEmbeddings, chosenPositions},
                                       axis=1);
        dimEmb += dimPos;
      }
      
      auto x = reshape(chosenEmbeddings, {dimBatch, dimEmb, dimWords});
      auto xMask = graph->constant(shape={dimBatch, 1, dimWords},
                                   init=inits::from_vector(subBatch->mask()));
      return std::make_tuple(x, xMask);
    }

  public:
    template <class ...Args>
    EncoderBase(Ptr<Config> options, Args ...args)
     : options_(options),
       prefix_(Get(keywords::prefix, "encoder", args...)),
       inference_(Get(keywords::inference, false, args...))
      {}

    virtual Ptr<EncoderState>
    build(Ptr<ExpressionGraph>, Ptr<data::CorpusBatch>, size_t = 0) = 0;
};

class DecoderBase {
  protected:
    Ptr<Config> options_;
    bool inference_{false};
    Ptr<LexProbs> lexProbs_;
    
  public:
    template <class ...Args>
    DecoderBase(Ptr<Config> options, Args ...args)
     : options_(options),
       inference_(Get(keywords::inference, false, args...)),
       lexProbs_(Get(keywords::lex_probs, nullptr, args...)) {}
    
    virtual std::tuple<Expr, Expr>
    groundTruth(Ptr<DecoderState> state,
                Ptr<ExpressionGraph> graph,
                Ptr<data::CorpusBatch> batch) {
      using namespace keywords;

      int dimVoc = options_->get<std::vector<int>>("dim-vocabs").back();
      int dimEmb = options_->get<int>("dim-emb");
      int dimPos = options_->get<int>("dim-pos");
      
      auto yEmb = Embedding("Wemb_dec", dimVoc, dimEmb)(graph);
      
      auto subBatch = batch->back();
      int dimBatch = subBatch->batchSize();
      int dimWords = subBatch->batchWidth();

      auto chosenEmbeddings = rows(yEmb, subBatch->indeces());
      
      if(dimPos) {
        int dimMax = options_->get<size_t>("max-length");
        std::vector<size_t> positions(subBatch->indeces().size());
        for(int i = 0; i < positions.size(); ++i) {
          int pos = i / dimBatch;
          positions[i] = pos < dimMax ? pos : dimMax;
        }
        
        auto yEmbPos = Embedding("Wpos_dec", dimMax + 1, dimPos)(graph);
        
        auto chosenPositions = rows(yEmbPos, positions);
        chosenEmbeddings = concatenate({chosenEmbeddings, chosenPositions},
                                       axis=1);
        dimEmb += dimPos;
      }
      
      auto y = reshape(chosenEmbeddings, {dimBatch, dimEmb, dimWords});

      auto yMask = graph->constant(shape={dimBatch, 1, dimWords},
                                   init=inits::from_vector(subBatch->mask()));
          
      auto yIdx = graph->constant(shape={(int)subBatch->indeces().size(), 1},
                                  init=inits::from_vector(subBatch->indeces()));
    
      auto yShifted = shift(y, {0, 0, 1, 0});
      
      state->setTargetEmbeddings(yShifted);
      
      return std::make_tuple(yMask, yIdx);
    }

    virtual void setLexicalProbabilites(Ptr<ExpressionGraph> graph,
                                        Ptr<data::CorpusBatch> batch) {
      if(lexProbs_)
        lexProbs_->resetLf(graph, batch);
    }
    
    virtual Ptr<DecoderState> startState(Ptr<EncoderState> encState) = 0;
    
    virtual void selectEmbeddings(Ptr<ExpressionGraph> graph,
                                  Ptr<DecoderState> state,
                                  const std::vector<size_t>& embIdx,
                                  size_t position=0) {
      using namespace keywords;
      
      int dimTrgEmb = options_->get<int>("dim-emb");
      int dimPosEmb = options_->get<int>("dim-pos");
      int dimTrgVoc = options_->get<std::vector<int>>("dim-vocabs").back();

      Expr selectedEmbs;
      if(embIdx.empty()) {
        selectedEmbs = graph->constant(shape={1, dimTrgEmb + dimPosEmb},
                                       init=inits::zeros);
      }
      else {
        auto yEmb = Embedding("Wemb_dec", dimTrgVoc, dimTrgEmb)(graph);
        selectedEmbs = rows(yEmb, embIdx);
        
        if(dimPosEmb) {
          int dimMax = options_->get<size_t>("max-length");
          auto yPos = Embedding("Wpos_dec", dimMax + 1, dimPosEmb)(graph);
          
          if(position > dimMax)
            position = dimMax;
          
          std::vector<size_t> positions(embIdx.size(), position);
          auto selectedPositions = rows(yPos, positions);
          
          selectedEmbs = concatenate({selectedEmbs, selectedPositions},
                                      axis=1);
          dimTrgEmb += dimPosEmb;
        }
        
        selectedEmbs = reshape(selectedEmbs,
                               {1, dimTrgEmb, 1, (int)embIdx.size()});
        
      }
      
      state->setTargetEmbeddings(selectedEmbs);
    }
    
    virtual Ptr<LexProbs> getLexProbs() {
      return lexProbs_;
    }
    
    virtual Ptr<DecoderState> step(Ptr<DecoderState>) = 0;
};

class EncoderDecoderBase {
  public:
    
    virtual void load(Ptr<ExpressionGraph>,
                      const std::string&) = 0;

    virtual void save(Ptr<ExpressionGraph>,
                      const std::string&) = 0;
    
    virtual void save(Ptr<ExpressionGraph>,
                      const std::string&, bool) = 0;

    virtual void selectEmbeddings(Ptr<ExpressionGraph> graph,
                                  Ptr<DecoderState> state,
                                  const std::vector<size_t>&,
                                  size_t position) = 0;
    
    virtual Ptr<DecoderState>
    step(Ptr<DecoderState>) = 0;

    virtual Expr build(Ptr<ExpressionGraph> graph,
                       Ptr<data::CorpusBatch> batch) = 0;
    
    virtual Ptr<EncoderBase> getEncoder() = 0;
    virtual Ptr<DecoderBase> getDecoder() = 0;
};

template <class Encoder, class Decoder>
class EncoderDecoder : public EncoderDecoderBase {
  protected:
    Ptr<Config> options_;
    Ptr<EncoderBase> encoder_;
    Ptr<DecoderBase> decoder_;
    Ptr<LexProbs> lexProbs_;
    bool inference_{false};

  public:

    template <class ...Args>
    EncoderDecoder(Ptr<Config> options, Args ...args)
     : options_(options),
       encoder_(New<Encoder>(options, args...)),
       decoder_(New<Decoder>(options, args...)),
       lexProbs_(Get(keywords::lex_probs, nullptr, args...)),
       inference_(Get(keywords::inference, false, args...))
    { }
    
    Ptr<EncoderBase> getEncoder() {
      return encoder_;
    }

    Ptr<DecoderBase> getDecoder() {
      return decoder_;
    }
    
    virtual void load(Ptr<ExpressionGraph> graph,
                       const std::string& name) {
      graph->load(name);
    }
    
    virtual void save(Ptr<ExpressionGraph> graph,
                      const std::string& name,
                      bool saveTranslatorConfig) {
      // ignore config for now
      graph->save(name);
      options_->saveModelParameters(name);
    }
    
    virtual void save(Ptr<ExpressionGraph> graph,
                      const std::string& name) {
      graph->save(name);
      options_->saveModelParameters(name);
    }
    
    virtual void clear(Ptr<ExpressionGraph> graph) {
      graph->clear();
      encoder_ = New<Encoder>(options_,
                              keywords::lex_probs=lexProbs_,
                              keywords::inference=inference_);
      decoder_ = New<Decoder>(options_,
                              keywords::lex_probs=lexProbs_,
                              keywords::inference=inference_);
    }

    virtual Ptr<DecoderState> startState(Ptr<ExpressionGraph> graph,
                                         Ptr<data::CorpusBatch> batch) {
      decoder_->setLexicalProbabilites(graph, batch);
      return decoder_->startState(encoder_->build(graph, batch));
    }
    
    virtual Ptr<DecoderState>
    step(Ptr<DecoderState> state) {
      return decoder_->step(state);
    }
    
    virtual void selectEmbeddings(Ptr<ExpressionGraph> graph,
                                  Ptr<DecoderState> state,
                                  const std::vector<size_t>& embIdx,
                                  size_t position) {
      return decoder_->selectEmbeddings(graph, state, embIdx, position);
    }

    virtual Expr build(Ptr<ExpressionGraph> graph,
                       Ptr<data::CorpusBatch> batch) {
      using namespace keywords;

      clear(graph);
      auto state = startState(graph, batch);
      
      Expr trgMask, trgIdx;
      std::tie(trgMask, trgIdx) = decoder_->groundTruth(state, graph, batch);
      
      auto nextState = step(state);
      
      auto cost = CrossEntropyCost("cost")(nextState->getProbs(),
                                           trgIdx, mask=trgMask);

      return cost;
    }

    Ptr<data::BatchStats> collectStats(Ptr<ExpressionGraph> graph) {
      auto stats = New<data::BatchStats>();
      
      size_t step = 10;
      size_t maxLength = options_->get<size_t>("max-length");
      size_t numFiles = options_->get<std::vector<std::string>>("train-sets").size();
      for(size_t i = step; i <= maxLength; i += step) {
        size_t batchSize = step;
        std::vector<size_t> lengths(numFiles, i);
        bool fits = true;
        do {
          auto batch = data::CorpusBatch::fakeBatch(lengths, batchSize);
          build(graph, batch);
          fits = graph->fits();
          if(fits)
            stats->add(batch);
          batchSize += step;
        }
        while(fits);
      }
      return stats;
    }
};

}