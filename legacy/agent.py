import config

from pydantic_ai import Agent, WebSearchTool
from pydantic_ai.models.openai import OpenAIChatModel
from pydantic_ai.providers.openai import OpenAIProvider

from dataclasses import dataclass
import tools
from pydantic_ai.common_tools.duckduckgo import duckduckgo_search_tool

@dataclass
class Assistant:
    agent: Agent
    deps: tools.ToolUsage

    @classmethod
    def create_agent(cls):
        provider = OpenAIProvider(base_url=config.base_url, api_key=config.api_key)
        model = OpenAIChatModel(model_name='qwen3.5-4b', provider=provider)
        prompt = '/no_think Reply in one short sentence'
        
        deps = tools.ToolUsage({})
        agent = Agent(model, system_prompt=prompt,
                #builtin_tools=[WebSearchTool()],
                tools=[tools.get_time, tools.get_weather, tools.play_music, duckduckgo_search_tool()],
                #tools=[tools.get_time, tools.get_weather, tools.play_music],
                      deps_type=tools.ToolUsage)
        return Assistant(agent, deps)

    def __call__(self, cmd):
        return self.agent.run_sync(cmd, deps=self.deps)

if __name__ == "__main__":
    llm = Assistant.create_agent()
    #result = llm('What is the time right now?')
    #print(result.output)
    #result = llm('What is the weather right now?')
    #print(result.output)
    #result = llm('Play the song Bon Jovi its my life')
    #print(result.output)
    result = llm('Who won US Open in Tennis in Mens singles in 2025?')
    print(result.output)
