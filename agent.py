import config

from pydantic_ai import Agent
from pydantic_ai.models.openai import OpenAIModel
from pydantic_ai.providers.openai import OpenAIProvider

import tools
from pydantic_ai.common_tools.duckduckgo import duckduckgo_search_tool
from dataclasses import dataclass

@dataclass
class Assistant:
    agent: Agent
    deps: tools.ToolUsage

    @classmethod
    def create_agent(cls):
        provider = OpenAIProvider(base_url=config.base_url, api_key=config.api_key)
        model = OpenAIModel(model_name='qwen3:4b', provider=provider)
        prompt = '/no_think Reply in one short sentence'
        
        deps = tools.ToolUsage({})
        agent = Agent(model, system_prompt=prompt,
                      tools=[tools.get_time, tools.get_weather, tools.play_music, duckduckgo_search_tool()],
                      deps_type=tools.ToolUsage)
        return Assistant(agent, deps)

    def __call__(self, cmd):
        return self.agent.run_sync(cmd, deps=self.deps)

if __name__ == "__main__":
    llm = Assistant.create_agent()
    #result = agent.run_sync('What is the time right now?')
    #print(result.output)
    #result = agent.run_sync('What is the weather right now?')
    #print(result.output)
    result = llm('Play the song Bon Jovi its my life')
    print(result.output)
    #result = agent.run_sync('Who won Monte Carlo Masters tournament in Mens singles in 2024?')
    #print(result.output)
