#pragma once

class ComputePipelineBuilder {
  public:
    ComputePipelineBuilder();
    ~ComputePipelineBuilder();

    ComputePipelineBuilder(const ComputePipelineBuilder&) = delete;
    ComputePipelineBuilder& operator=(const ComputePipelineBuilder&) = delete;
    ComputePipelineBuilder(ComputePipelineBuilder&&) = delete;
    ComputePipelineBuilder& operator=(ComputePipelineBuilder&&) = delete;

  private:
};
